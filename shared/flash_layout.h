/* ============================================================================
 * flash_layout.h —— Bootloader 与 App 之间【唯一】的 Flash 契约
 *
 * ── 为什么要有这个文件 ────────────────────────────────────────────────────
 * Bootloader 和 App 是两个独立固件、两个独立 Keil 工程：
 *   - Bootloader 链在 0x08000000，读标志区、搬运固件、跳转
 *   - App       链在 0x08010000，OTA 时擦写 A/B 区、写标志区
 * 它们必须对一个问题的答案完全一致：「哪个地址是哪块、标志区长什么样」。
 *
 * 之前这两份定义各写了一遍（bootloader/Inc/boot_cfg.h 和 BSP/Inc/flash_if.h），
 * 靠注释警告"改一处要同步改另一处"。这是最危险的一类约定：
 * 编译器不会帮你对账，结构体差一个字段、地址差一个扇区，
 * 表现就是"OTA 显示成功，重启后变砖"，而且极难定位。
 *
 * 现在收敛成这一份。两边都 include 它，物理上不可能不一致。
 *
 * ── 本文件的三条纪律 ──────────────────────────────────────────────────────
 *   ① 只放【契约】：地址常量 + 标志结构体 + 魔数 + 状态枚举
 *   ② 不许放实现，不许依赖 HAL / FreeRTOS / main.h —— 只能 include <stdint.h>
 *      （Bootloader 不用 HAL，一旦依赖 HAL 它就编译不过了）
 *   ③ 改动这里 = 同时改两个固件，必须一起重新编译烧录
 *
 * ── Keil 配置（两个工程各加一次，一次性） ─────────────────────────────────
 *   App 工程        Options → C/C++ → Include Paths 加：..\..\shared
 *   Bootloader 工程 Options → C/C++ → Include Paths 加：..\..\shared
 *   （路径相对工程文件所在目录，即各自 MDK-ARM 文件夹）
 * ==========================================================================*/
#ifndef __FLASH_LAYOUT_H__
#define __FLASH_LAYOUT_H__

#include <stdint.h>

/* ============================================================================
 * 一、分区表（STM32F407ZGT6，1MB Flash，单 Bank）
 *
 * 扇区大小不均匀，所以只能查表换算，不能用除法：
 *   S0 0x08000000 16KB   S1 0x08004000 16KB   S2 0x08008000 16KB
 *   S3 0x080C0000 16KB   S4 0x08010000 64KB   S5~S11 各 128KB
 *
 * ★ 分区边界必须落在扇区起点上：擦除的最小单位是整个扇区，
 *   边界没对齐就会"擦 A 区时把 App 尾巴一起擦掉"。
 * ==========================================================================*/
#define BOOT_ADDR           0x08000000u   /* Bootloader  S0+S1       32KB */
#define BOOT_SIZE           (32u * 1024u)

#define FLAG_ADDR_MAIN      0x08008000u   /* 标志区主块  S2          16KB */
#define FLAG_ADDR_BACKUP    0x0800C000u   /* 标志区备份  S3          16KB */
#define FLAG_SIZE           (16u * 1024u)

#define APP_ADDR            0x08010000u   /* 运行区 App  S4+S5+S6   320KB */
#define APP_SIZE            (320u * 1024u)

#define BANK_A_ADDR         0x08060000u   /* A 暂存区    S7+S8      256KB */
#define BANK_B_ADDR         0x080A0000u   /* B 暂存区    S9+S10     256KB */
#define BANK_SIZE           (256u * 1024u)

/* 固件大小上限 = App 区容量。上位机发超了要在这里被挡住，
   否则会写到 BANK_A 里去，把 OTA 暂存区污染成垃圾。 */
#define FW_SIZE_MAX         APP_SIZE

/* ============================================================================
 * 二、启动标志（放在标志区，Bootloader 靠它决定跳哪儿）
 *
 * 固定 24 字节。★ 尾部带一个自己的 crc32：
 * 标志区本身在 Flash 里，写的时候掉电可能只写进去一半。
 * 没有自检的话 Bootloader 会拿着残缺的标志去做搬运决策 → 直接变砖。
 *
 * ⚠️ 改这个结构体必须 Boot 和 App 一起重编译，
 *    否则两边对 24 个字节的解释会错位。
 * ==========================================================================*/
#define BOOT_FLAG_MAGIC     0x5A5AA5A5u   /* 魔数：标志区数据有效的凭证 */

/* upgrade_flag 取值 */
#define UPGRADE_NONE        0u            /* 无升级任务，直接跑 App */
#define UPGRADE_PENDING     1u            /* Bank 里有新固件待搬运 */
#define UPGRADE_TRIAL       2u            /* 试运行中，等待 App 确认（预留） */

/* 连续启动失败到这个次数就放弃搬运，老实跑老固件（自动回滚） */
#define BOOT_COUNT_MAX      3u

typedef struct {
    uint32_t magic;         /* = BOOT_FLAG_MAGIC 表示有效 */
    uint32_t version;       /* 固件版本，如 0x00010000 = v1.0 */
    uint32_t fw_size;       /* 固件字节数 */
    uint32_t fw_crc32;      /* 整个 bin 的 CRC32 */
    uint8_t  active_bank;   /* 当前跑哪个 Bank：0=A, 1=B */
    uint8_t  upgrade_flag;  /* 见上面的 UPGRADE_* */
    uint8_t  boot_count;    /* 连续启动失败次数 */
    uint8_t  reserved;      /* 保持 4 字节对齐 */
    uint32_t crc32;         /* 以上字段（不含本字段）的 CRC32 */
} BootFlag_t;

/* ============================================================================
 * 三、读 Flash 的地址换算
 *
 * STM32 是 XIP 架构：内部 Flash 直接挂在 0x08000000 地址空间上，
 * 所以"读"就是把地址当指针解引用，不需要任何驱动函数。
 * ==========================================================================*/
#define FLASH_PTR8(addr)    ((volatile uint8_t  *)(addr))
#define FLASH_PTR32(addr)   ((volatile uint32_t *)(addr))
#define FLASH_RD32(addr)    (*FLASH_PTR32(addr))

#endif /* __FLASH_LAYOUT_H__ */
