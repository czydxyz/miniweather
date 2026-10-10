/* Bootloader 专用 Flash 驱动（直接操作寄存器，不用 HAL）
 * [约束] 解锁：FLASH->KEYR 连写 0x45670123、0xCDEF89AB。
 * [约束] 擦除最小单位是整个扇区；写前目标必须已擦除。
 */
/* ★ FLASH / RCC 寄存器符号都在设备头里，必须包含，否则 FLASH->KEYR 等全部 undeclared */
#include "stm32f4xx.h"

#include "flash_boot.h"
#include "crc32.h"
#include <string.h>

/* 扇区表：大小不均匀，只能查表 */
static const uint32_t s_sector_addr[12] = {
    0x08000000u, 0x08004000u, 0x08008000u, 0x0800C000u,
    0x08010000u, 0x08020000u, 0x08040000u, 0x08060000u,
    0x08080000u, 0x080A0000u, 0x080C0000u, 0x080E0000u
};
static const uint32_t s_sector_size[12] = {
    16u*1024u, 16u*1024u, 16u*1024u, 16u*1024u,
    64u*1024u,
    128u*1024u, 128u*1024u, 128u*1024u, 128u*1024u,
    128u*1024u, 128u*1024u, 128u*1024u
};

/* 解锁 / 上锁 */
int FLASH_Unlock(void)
{
    if ((FLASH->CR & FLASH_CR_LOCK) == 0u) return 0;   /* 已经解锁 */

    FLASH->KEYR = 0x45670123u;
    FLASH->KEYR = 0xCDEF89ABu;

    /* 清掉上一次可能残留的错误标志（写 1 清零） */
    FLASH->SR = (FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR |
                 FLASH_SR_WRPERR | FLASH_SR_OPERR  | FLASH_SR_EOP);

    return (FLASH->CR & FLASH_CR_LOCK) ? 1 : 0;
}

void FLASH_Lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

/* 地址 ↔ 扇区 */
uint32_t FLASH_SectorOf(uint32_t addr)
{
    uint32_t i;
    if (addr < 0x08000000u || addr >= 0x08100000u) return 0xFFFFFFFFu;
    for (i = 0; i < 12u; i++) {
        if (addr < s_sector_addr[i] + s_sector_size[i]) return i;
    }
    return 0xFFFFFFFFu;
}

uint32_t FLASH_SectorAddr(uint32_t sector)
{
    return (sector < 12u) ? s_sector_addr[sector] : 0xFFFFFFFFu;
}

uint32_t FLASH_SectorSize(uint32_t sector)
{
    return (sector < 12u) ? s_sector_size[sector] : 0u;
}

/* 擦一个扇区 */
static int EraseOneSector(uint32_t sector)
{
    uint32_t tmo;

    /* PSIZE 擦除时也必须设：00 = 字节 */
    FLASH->CR &= ~FLASH_CR_PSIZE_Msk;
    FLASH->CR &= ~FLASH_CR_SNB_Msk;
    FLASH->CR |= FLASH_CR_SER | (sector << FLASH_CR_SNB_Pos);
    FLASH->CR |= FLASH_CR_STRT;

    /* 擦一个 128KB 扇区规格书标称 1~2 秒，超时给足 */
    tmo = 0u;
    while (FLASH->SR & FLASH_SR_BSY) {
        if (++tmo > 0x2000000u) {                 /* 约几秒，够用 */
            FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB_Msk);
            return 2;
        }
    }

    FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB_Msk);
    /* 检查有没有报错 */
    if (FLASH->SR & (FLASH_SR_PGSERR | FLASH_SR_PGPERR |
                     FLASH_SR_PGAERR | FLASH_SR_WRPERR)) {
        FLASH->SR = (FLASH_SR_PGSERR | FLASH_SR_PGPERR |
                     FLASH_SR_PGAERR | FLASH_SR_WRPERR);
        return 3;
    }
    return 0;
}

/* 按地址区间擦除：算出首尾扇区，逐个擦 */
int FLASH_EraseRange(uint32_t addr, uint32_t len)
{
    uint32_t s, e, sec;
    int      r;

    if (len == 0u) return 0;
    s = FLASH_SectorOf(addr);
    e = FLASH_SectorOf(addr + len - 1u);
    if (s == 0xFFFFFFFFu || e == 0xFFFFFFFFu) return 1;

    if (FLASH_Unlock() != 0) return 1;

    for (sec = s; sec <= e; sec++) {
        r = EraseOneSector(sec);
        if (r != 0) { FLASH_Lock(); return (r == 3) ? 3 : 2; }
    }

    FLASH_Lock();
    return 0;
}

/* 写入 */
int FLASH_Write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t i = 0u;
    uint32_t tmo;

    if (len == 0u) return 0;
    if (addr < 0x08000000u || (addr + len) > 0x08100000u) return 1;

    if (FLASH_Unlock() != 0) return 1;

    FLASH->CR &= ~FLASH_CR_PSIZE_Msk;      /* 00 = 字节编程 */

    /* 先按双字节/四字节对齐地写整块，能快好几倍。
       这里用最保守的【逐字节】，因为数据可能来自非对齐的 RAM 缓冲，
       而 Bootloader 一秒两秒的差别根本不重要，正确性优先。 */
    for (i = 0u; i < len; i++) {
        FLASH->CR |= FLASH_CR_PG;
        *FLASH_PTR8(addr + i) = data[i];

        tmo = 0u;
        while (FLASH->SR & FLASH_SR_BSY) {
            if (++tmo > 0x100000u) {
                FLASH->CR &= ~FLASH_CR_PG;
                FLASH_Lock();
                return 3;
            }
        }
        FLASH->CR &= ~FLASH_CR_PG;

        if (FLASH->SR & (FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR)) {
            FLASH->SR = (FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR);
            FLASH_Lock();
            return 3;
        }
    }

    FLASH_Lock();
    return 0;
}

/* 启动标志：乒乓读写 */
/* crc32 字段之前有多少字节。
   ★ 必须用 offsetof，不能手写 20：结构体一旦改动，
     手写的常量不会跟着变，Boot 和 App 就可能算出不同的 CRC 范围 → 永远校验失败。
     （和 App 侧 BSP/Src/flash_if.c 用的是同一个写法，保持一致） */
#include <stddef.h>
#define FLAG_CRC_OFFSET   ((uint32_t)offsetof(BootFlag_t, crc32))

/* 校验一份标志：魔数对 + 自身 CRC 对 */
static int FlagValid(const BootFlag_t *f)
{
    if (f->magic != BOOT_FLAG_MAGIC) return 0;
    if (CRC32_Calc((const uint8_t *)f, FLAG_CRC_OFFSET) != f->crc32) return 0;
    return 1;
}

int FLASH_ReadBootFlag(BootFlag_t *flag)
{
    BootFlag_t *pmain = (BootFlag_t *)FLAG_ADDR_MAIN;
    BootFlag_t *pback = (BootFlag_t *)FLAG_ADDR_BACKUP;
    BootFlag_t  tmp;

    /* 先试主块 S2 */
    memcpy(&tmp, (const void *)pmain, sizeof(tmp));
    if (FlagValid(&tmp)) { *flag = tmp; return 0; }

    /* 主块坏了退备份块 S3 */
    memcpy(&tmp, (const void *)pback, sizeof(tmp));
    if (FlagValid(&tmp)) { *flag = tmp; return 0; }

    return 1;      /* 两份都无效：全新的芯片，或者标志区被擦过 */
}

/* 写一份标志到指定槽位（调用前已解锁） */
static int WriteFlagSlot(uint32_t slot_addr, const BootFlag_t *flag)
{
    BootFlag_t tmp;
    int        r;

    /* ① 擦掉这个 16KB 扇区 */
    r = (int)EraseOneSector(FLASH_SectorOf(slot_addr));
    if (r != 0) return 2;

    /* ② 算自身 CRC 再整体写入 */
    memcpy(&tmp, flag, sizeof(tmp));
    tmp.magic = BOOT_FLAG_MAGIC;
    tmp.crc32 = CRC32_Calc((const uint8_t *)&tmp, FLAG_CRC_OFFSET);

    /* FLASH_Write 内部会自己解锁/上锁，这里先上锁交出去 */
    FLASH_Lock();
    r = FLASH_Write(slot_addr, (const uint8_t *)&tmp, (uint32_t)sizeof(tmp));
    if (r != 0) return 3;

    /* ③ 写回读校验：Flash 写进去不一定是你想要的，读回来对一遍 */
    {
        BootFlag_t chk;
        memcpy(&chk, (const void *)slot_addr, sizeof(chk));
        if (memcmp(&chk, &tmp, sizeof(chk)) != 0) return 4;
    }
    return 0;
}

/* 乒乓写：先备后主。
   ★ 顺序不能反：先写备份，保证"新数据至少有一份完整"之后才动主块。
     反过来写的话，擦完主块、还没写完备块时掉电，两份都是坏的。 */
int FLASH_WriteBootFlag(const BootFlag_t *flag)
{
    int r;

    if (FLASH_Unlock() != 0) return 1;
    r = WriteFlagSlot(FLAG_ADDR_BACKUP, flag);
    if (r != 0) { FLASH_Lock(); return r; }

    if (FLASH_Unlock() != 0) return 1;
    r = WriteFlagSlot(FLAG_ADDR_MAIN, flag);
    FLASH_Lock();
    return r;
}
