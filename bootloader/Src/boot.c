/* Bootloader 主流程：判标志 -> 先验后擦 -> 搬运 -> 再验 -> 跳转
 * [约束] 本工程跑 HSI 16MHz（非 168MHz），USART BRR 须按 16MHz 计算。
 * [约束] 跳转四步：__disable_irq() -> 停 SysTick -> SCB->VTOR + MSP -> 跳转。
 * [约束] App 必须在自身开头 __enable_irq()，否则 PRIMASK=1 被带入，HAL_Delay 死等。
 * [约束] 搬运失败必须保留 UPGRADE_PENDING 并 boot_count++，超 BOOT_COUNT_MAX 才放弃重试。
 * [约束] 必须先校验源固件 CRC32 再擦运行区，校验失败绝不写入。
 */
#include "stm32f4xx.h"

#include "boot.h"
#include "boot_cfg.h"
#include "flash_boot.h"
#include "crc32.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern uint32_t SystemCoreClock;      /* 由 system_stm32f4xx.c 维护 */

static uint32_t Boot_GetPclk2(void)
{
    uint32_t hclk  = SystemCoreClock;
    uint32_t ppre2 = (RCC->CFGR & RCC_CFGR_PPRE2) >> RCC_CFGR_PPRE2_Pos;
    /* PPRE2 编码：0xx = 不分频，100 = /2，101 = /4，110 = /8，111 = /16 */
    static const uint8_t div[8] = {1u, 1u, 1u, 1u, 2u, 4u, 8u, 16u};

    return hclk / (uint32_t)div[ppre2 & 0x7u];
}

void Boot_LogInit(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;      /* GPIOA 时钟 */
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;     /* USART1 时钟 */

    /* PA9(TX) PA10(RX) 配成复用功能 AF7 */
    GPIOA->MODER  &= ~((3u << (9u * 2u)) | (3u << (10u * 2u)));
    GPIOA->MODER  |=  ((2u << (9u * 2u)) | (2u << (10u * 2u)));
    GPIOA->OTYPER &= ~(1u << 9u);                                  /* 推挽 */
    GPIOA->OSPEEDR |= ((3u << (9u * 2u)) | (3u << (10u * 2u)));    /* 高速 */
    /* AFR[1] 管 pin8~15，PA9 在 AFR[1] 的第 1 组，PA10 在第 2 组 */
    GPIOA->AFR[1] &= ~((0xFu << ((9u - 8u) * 4u)) | (0xFu << ((10u - 8u) * 4u)));
    GPIOA->AFR[1] |=  ((7u  << ((9u - 8u) * 4u)) | (7u  << ((10u - 8u) * 4u)));

    /* 波特率：OVER8=0 时 BRR = (PCLK2 + baud/2) / baud，四舍五入 */
    USART1->BRR = (uint16_t)((Boot_GetPclk2() + 115200u / 2u) / 115200u);
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static void Boot_Putc(char c)
{
    while ((USART1->SR & USART_SR_TXE) == 0u) { }
    USART1->DR = (uint8_t)c;
}

void Boot_Log(const char *fmt, ...)
{
    char     buf[128];
    va_list  ap;
    int      n, i;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    for (i = 0; i < n; i++) Boot_Putc(buf[i]);
}


static int AppVectorValid(uint32_t addr)
{
    uint32_t sp = FLASH_RD32(addr + 0u);
    uint32_t pc = FLASH_RD32(addr + 4u);

    /* ① 栈顶必须落在 SRAM(0x20000000 起 128KB) 或 CCM(0x10000000 起 64KB) 里。
          注意 F407 的 SRAM 是 128KB+64KB CCM，不是 192KB 连续。 */
    if (!((sp >= 0x20000000u && sp <= 0x20020000u) ||
          (sp >= 0x10000000u && sp <= 0x10010000u))) return 0;

    /* ② 复位向量必须在 App 区内，且最低位是 1（Thumb 状态标志） */
    if ((pc & 1u) == 0u) return 0;
    {
        uint32_t real = pc & ~1u;
        if (real < (addr + 0x100u) || real > (addr + APP_SIZE)) return 0;
    }

    /* ③ 全是 0xFF 说明压根没烧过程序（擦后状态） */
    if (sp == 0xFFFFFFFFu || pc == 0xFFFFFFFFu) return 0;

    return 1;
}

static int CopyBankToApp(uint32_t src, uint32_t size, uint32_t expect_crc)
{
    uint32_t crc;
    int      r;

    if (size == 0u || size > APP_SIZE) return -1;
    if (src != BANK_A_ADDR && src != BANK_B_ADDR) return -1;

    /* ① 先在源头上算 CRC —— 必须验完再擦，顺序反了就是砖 */
    Boot_Log("  crc calc ... ");
    crc = CRC32_CalcFlash(src, size);
    Boot_Log("0x%08lX (want 0x%08lX)\r\n",
             (unsigned long)crc, (unsigned long)expect_crc);
    if (crc != expect_crc) return -2;

    /* ② 擦运行区。S4(64KB)+S5+S6(128KB×2) 共 3 个扇区，规格书标称 1~2 秒/扇区 */
    Boot_Log("  erase app ... ");
    r = FLASH_EraseRange(APP_ADDR, size);
    Boot_Log("%s\r\n", (r == 0) ? "ok" : "FAIL");
    if (r != 0) return -3;

    /* ③ 逐段搬。一次搬 1KB，避免栈上开大缓冲（Bootloader 栈只有几 KB） */
    Boot_Log("  copy ... ");
    {
        /* 缓冲只开 256 字节：Bootloader 的栈通常只有 1~4KB，
           开 1KB 的局部缓冲容易直接把栈撑爆（表现为莫名其妙 HardFault）。
           反正一次搬多少不影响正确性，小一点更稳。 */
        uint8_t  chunk[256];
        uint32_t off = 0u;

        while (off < size) {
            uint32_t n = size - off;
            if (n > sizeof(chunk)) n = sizeof(chunk);

            memcpy(chunk, (const void *)(src + off), n);

            r = FLASH_Write(APP_ADDR + off, chunk, n);
            if (r != 0) { Boot_Log("FAIL@0x%08lX\r\n", (unsigned long)off); return -4; }

            /* 每搬一块就对一遍，早点发现 Flash 写坏 */
            if (memcmp((const void *)(APP_ADDR + off), chunk, n) != 0) {
                Boot_Log("MISMATCH@0x%08lX\r\n", (unsigned long)off);
                return -5;
            }
            off += n;
        }
    }
    Boot_Log("ok\r\n");

    /* ④ 搬完再整体验一次，确认运行区和源头一致 */
    crc = CRC32_CalcFlash(APP_ADDR, size);
    Boot_Log("  verify 0x%08lX\r\n", (unsigned long)crc);
    if (crc != expect_crc) return -5;

    return 0;
}

static void Boot_Jump(uint32_t addr)
{
    uint32_t   sp = FLASH_RD32(addr + 0u);
    uint32_t   pc = FLASH_RD32(addr + 4u);
    uint32_t   i;
    void     (*app_reset)(void);

    /* ① 关中断 + 清掉所有挂起位 */
    __disable_irq();
    for (i = 0u; i < 8u; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;      /* 禁能 */
        NVIC->ICPR[i] = 0xFFFFFFFFu;      /* 清挂起 */
    }

    /* ② 停 SysTick，别让它在切换途中插一脚 */
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL  = 0u;

    /* ③ 向量表重定位 + 换栈 */
    SCB->VTOR = addr;
    __DSB();
    __ISB();
    __set_MSP(sp);

    /* ④ 跳。这一跳之后就不再回来了 */
    app_reset = (void (*)(void))pc;
    app_reset();

    while (1u) { }        /* 理论上到不了 */
}

void Boot_DelayMs(uint32_t ms)
{
    uint32_t clk = SystemCoreClock;
    uint32_t n;

    if (clk == 0u) clk = 16000000u;      /* SystemCoreClock 还没被赋值时的兜底 */
    n = (clk / 1000u) * ms / 4u;
    while (n--) { __NOP(); }
}

static int ForceKeyPressed(void)
{
    uint32_t elapsed;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN;               /* 开 GPIOE 时钟 */

    GPIOE->MODER &= ~(3u << (4u * 2u));                /* 00 = 输入 */
    GPIOE->PUPDR &= ~(3u << (4u * 2u));
    GPIOE->PUPDR |=  (1u << (4u * 2u));                /* 01 = 上拉 */
    GPIOE->OSPEEDR &= ~(3u << (4u * 2u));

    /* 开上拉后要等几个周期电平才稳定，不延时会读到浮空值 */
    Boot_DelayMs(1u);

    /* 在上电窗口内反复采样：用户不必"先按住再上电"，上电后按也来得及 */
    for (elapsed = 0u; elapsed < BOOT_FORCE_KEY_MS; elapsed += 20u) {
        if ((GPIOE->IDR & (1u << 4u)) == 0u) {         /* 低 = 按下 */
            Boot_DelayMs(10u);                         /* 消抖 */
            if ((GPIOE->IDR & (1u << 4u)) == 0u) return 1;
        }
        Boot_DelayMs(20u);
    }
    return 0;
}

/* 五、主流程 */
void Boot_Run(void)
{
    BootFlag_t flag;
    int        r;
    int        flag_ok;
    int        force;

    Boot_Log("\r\n==== MiniWeather Bootloader ====\r\n");
    Boot_Log("boot @0x%08lX, app @0x%08lX\r\n",
             (unsigned long)BOOT_ADDR, (unsigned long)APP_ADDR);

    /* ---- ⓪ 上电窗口内按住 KEY0 = 强制重装（救砖） ---- */
    force = ForceKeyPressed();
    if (force) Boot_Log("[KEY ] KEY0 held -> FORCE reinstall\r\n");

    /* ---- ① 读标志 ---- */
    memset(&flag, 0, sizeof(flag));
    flag_ok = (FLASH_ReadBootFlag(&flag) == 0) ? 1 : 0;

    if (!flag_ok) {
        /* 标志区无效：可能是全新芯片，或者标志被擦过。
           这种情况不搬运，直接跑现有的 App —— 能跑就跑，跑不了再说。 */
        Boot_Log("flag: none/invalid -> run app as-is\r\n");
        if (force) Boot_Log("[KEY ] but no valid flag, cannot force\r\n");
    } else {
        Boot_Log("flag: ver=0x%08lX bank=%u upg=%u bootcnt=%u size=%lu\r\n",
                 (unsigned long)flag.version,
                 (unsigned int)flag.active_bank,
                 (unsigned int)flag.upgrade_flag,
                 (unsigned int)flag.boot_count,
                 (unsigned long)flag.fw_size);

        /* 强制键：不管标志现在是什么状态，都要重新装一次，
           并把失败计数清零（否则撞上 BOOT_COUNT_MAX 就永远装不了了） */
        if (force) {
            flag.upgrade_flag = UPGRADE_PENDING;
            flag.boot_count   = 0u;
        }

        /* ---- ② 有升级任务就搬运 ---- */
        if (flag.upgrade_flag == UPGRADE_PENDING &&
            flag.fw_size > 0u && flag.fw_size <= APP_SIZE &&
            flag.boot_count < BOOT_COUNT_MAX) {

            uint32_t src = (flag.active_bank == 0u) ? BANK_A_ADDR : BANK_B_ADDR;

            Boot_Log("upgrade pending: bank %u -> app\r\n",
                     (unsigned int)flag.active_bank);

            r = CopyBankToApp(src, flag.fw_size, flag.fw_crc32);

            if (r == 0) {
                /* 成功：清掉升级标志，这次升级才算真正落地 */
                flag.upgrade_flag = UPGRADE_NONE;
                flag.boot_count   = 0u;
                if (FLASH_WriteBootFlag(&flag) != 0) {
                    Boot_Log("!! flag write fail\r\n");
                } else {
                    Boot_Log("upgrade DONE\r\n");
                }

            } else if (r == -2) {
                /* ★ 分类处理：源固件本身 CRC 就错 —— 下载过程就没成功。
                   重试一百次也是错的，直接放弃，保住能跑的老固件。 */
                flag.upgrade_flag = UPGRADE_NONE;
                flag.boot_count   = 0u;
                (void)FLASH_WriteBootFlag(&flag);
                Boot_Log("!! bank CRC bad -> abort, keep old app\r\n");

            } else {
                if (flag.boot_count < 0xFFu) flag.boot_count++;
                (void)FLASH_WriteBootFlag(&flag);
                Boot_Log("!! copy fail(%d) -> KEEP pending, retry next boot\r\n", r);
                Boot_Log("!! (boot_count=%u/%u)\r\n",
                         (unsigned int)flag.boot_count,
                         (unsigned int)BOOT_COUNT_MAX);
            }
        } else if (flag.boot_count >= BOOT_COUNT_MAX) {
            /* 重试太多次还不成功，说明不是偶发故障，放弃 */
            Boot_Log("boot_count hit max -> give up, run whatever in app\r\n");
        }
    }

    /* ---- ③ 检查并跳转 ---- */
    if (!AppVectorValid(APP_ADDR)) {
        Boot_Log("!! APP invalid @0x%08lX, HALT\r\n", (unsigned long)APP_ADDR);
        Boot_Log("!! reflash app via SWD\r\n");
        return;                       /* 返回后 main 里死循环，等救援 */
    }

    Boot_Log("jump to app...\r\n");
    /* 稍等一下让最后几个字节发完（115200 下 1ms 约 11 字节） */
    for (r = 0; r < 200000; r++) { __NOP(); }

    Boot_Jump(APP_ADDR);
}
