/* 内部 Flash 擦写（HAL 版，App 侧）
 * [约束] 擦除前必须 __HAL_FLASH_CLEAR_FLAG 清掉所有挂起标志。
 * [约束] FLASH_EraseInitTypeDef.Banks 必须显式填 FLASH_BANK_1，不填会静默失败。
 * [约束] 写前目标区域必须已擦成 0xFF；采用 TYPEPROGRAM_BYTE 字节编程。
 * [约束] 擦/写前解锁、完成后上锁。
 */
#include "flash_if.h"
#include <string.h>
#include <stddef.h>     /* offsetof */

/* crc32 字段之前有多少字节 —— 用 offsetof 而不是手算，结构体改了也不会错 */
#define FLAG_CRC_OFFSET      ((uint32_t)offsetof(BootFlag_t, crc32))


/* 本模块自带一份 CRC32，避免为了算 24 字节的标志校验去依赖 comm.c */
static uint32_t Crc32Step(uint32_t crc, uint8_t b)
{
    uint8_t i;
    crc ^= b;
    for (i = 0; i < 8; i++) {
        if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
        else          crc = (crc >> 1);
    }
    return crc;
}

static uint32_t Crc32Calc(const uint8_t *d, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu, i;
    for (i = 0; i < len; i++) crc = Crc32Step(crc, d[i]);
    return crc ^ 0xFFFFFFFFu;
}

/*
 * 一、地址 ↔ 扇区换算
 * F4 的扇区不是一样大的（前 4 个 16KB，第 5 个 64KB，后面全是 128KB），
 * 所以只能查表，不能用除法。
  */
static const uint32_t s_sector_addr[12] = {
    0x08000000u, 0x08004000u, 0x08008000u, 0x0800C000u,
    0x08010000u, 0x08020000u, 0x08040000u, 0x08060000u,
    0x08080000u, 0x080A0000u, 0x080C0000u, 0x080E0000u
};
static const uint32_t s_sector_size[12] = {
    16u * 1024u, 16u * 1024u, 16u * 1024u, 16u * 1024u,
    64u * 1024u,
    128u * 1024u, 128u * 1024u, 128u * 1024u, 128u * 1024u,
    128u * 1024u, 128u * 1024u, 128u * 1024u
};

uint32_t FLASH_IF_SectorOf(uint32_t addr)
{
    uint32_t i;
    if (addr < 0x08000000u || addr >= 0x08100000u) return 0xFFFFFFFFu;
    for (i = 0; i < 12u; i++) {
        if (addr < s_sector_addr[i] + s_sector_size[i]) return i;
    }
    return 0xFFFFFFFFu;
}

uint32_t FLASH_IF_SectorAddr(uint32_t sector)
{
    if (sector >= 12u) return 0xFFFFFFFFu;
    return s_sector_addr[sector];
}

uint32_t FLASH_IF_SectorSize(uint32_t sector)
{
    if (sector >= 12u) return 0;
    return s_sector_size[sector];
}

/* 二、擦除 */
static int EraseSectors(uint32_t start_sector, uint32_t nb)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t               sector_err = 0;
    HAL_StatusTypeDef      st;

    if (start_sector >= 12u || nb == 0 || (start_sector + nb) > 12u) return 1;

    /* ★ 清掉所有挂起的 Flash 状态位，否则 HAL 会以为上一操作还没完 */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    erase.TypeErase   = FLASH_TYPEERASE_SECTORS;
    /* F407 只有 Bank1。这个字段在"整片擦除"时才真正起作用，
       扇区擦除时 HAL 不看它，但不填的话 assert（开了 USE_FULL_ASSERT 时）
       会拿未初始化的值去比较，属于典型的"能跑但埋雷"。 */
    erase.Banks       = FLASH_BANK_1;
    erase.Sector      = start_sector;
    erase.NbSectors   = nb;
    /* VOLTAGE_RANGE_3 = 2.7~3.6V，按字节/字/双字编程都行。
       探索者板子是 3.3V，填 3。填错会导致编程时硬件报错。 */
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    HAL_FLASH_Unlock();
    st = HAL_FLASHEx_Erase(&erase, &sector_err);
    HAL_FLASH_Lock();

    return (st == HAL_OK) ? 0 : 2;
}

int FLASH_IF_EraseSectorByAddr(uint32_t addr)
{
    uint32_t sec = FLASH_IF_SectorOf(addr);
    if (sec == 0xFFFFFFFFu) return 1;
    return EraseSectors(sec, 1u);
}

/* 擦一段地址范围：把覆盖到的扇区全擦掉。
   ★ 注意：这会连"范围两端所在的整个扇区"一起擦，不是只擦 len 字节。
      比如擦 [0x08060100, +200 字节]，实际会把 Sector7 整个 128KB 擦掉。 */
int FLASH_IF_EraseRange(uint32_t addr, uint32_t len)
{
    uint32_t first, last;
    if (len == 0) return 0;
    first = FLASH_IF_SectorOf(addr);
    last  = FLASH_IF_SectorOf(addr + len - 1u);
    if (first == 0xFFFFFFFFu || last == 0xFFFFFFFFu) return 1;
    return EraseSectors(first, last - first + 1u);
}

int FLASH_IF_Write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    HAL_StatusTypeDef st = HAL_OK;

    if (data == NULL && len != 0) return 1;
    if (FLASH_IF_SectorOf(addr) == 0xFFFFFFFFu) return 1;
    if (len == 0) return 0;

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    HAL_FLASH_Unlock();
    for (i = 0; i < len; i++) {
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, addr + i, data[i]);
        if (st != HAL_OK) break;
    }
    HAL_FLASH_Lock();

    if (st != HAL_OK) return 3;

    /* 写回校验：Flash 写失败有时会静默发生（电压不足 / 没擦就写），
       读回来比一遍是最便宜的保险。 */
    {
        const uint8_t *src = data;
        volatile uint8_t *dst = FLASH_PTR8(addr);
        for (i = 0; i < len; i++) {
            if (dst[i] != src[i]) return 4;
        }
    }
    return 0;
}

void FLASH_IF_Read(uint32_t addr, uint8_t *data, uint32_t len)
{
    uint32_t i;
    volatile uint8_t *src = FLASH_PTR8(addr);
    if (data == NULL || len == 0) return;
    for (i = 0; i < len; i++) data[i] = src[i];
}

static int WriteFlagTo(uint32_t slot_addr, const BootFlag_t *flag)
{
    uint8_t  raw[sizeof(BootFlag_t)];
    int      r;

    memcpy(raw, flag, sizeof(BootFlag_t));

    r = FLASH_IF_EraseSectorByAddr(slot_addr);
    if (r != 0) return r;

    return FLASH_IF_Write(slot_addr, raw, (uint32_t)sizeof(BootFlag_t));
}

int FLASH_IF_WriteBootFlag(const BootFlag_t *flag)
{
    BootFlag_t f = *flag;
    int        r;

    if (flag == NULL) return 1;

    f.magic = BOOT_FLAG_MAGIC;
    /* crc32 覆盖除自己以外的所有字段 */
    f.crc32 = Crc32Calc((const uint8_t *)&f, FLAG_CRC_OFFSET);

    /* ① 先写备份块 S3 —— 就算写主块时掉电，S3 里也已经是新标志了 */
    r = WriteFlagTo(FLAG_ADDR_BACKUP, &f);
    if (r != 0) return r;

    /* ② 再写主块 S2 */
    r = WriteFlagTo(FLAG_ADDR_MAIN, &f);
    if (r != 0) return r;

    return 0;
}

static int ReadFlagFrom(uint32_t slot_addr, BootFlag_t *flag)
{
    uint8_t raw[sizeof(BootFlag_t)];
    uint32_t crc;

    FLASH_IF_Read(slot_addr, raw, (uint32_t)sizeof(BootFlag_t));
    memcpy(flag, raw, sizeof(BootFlag_t));

    if (flag->magic != BOOT_FLAG_MAGIC) return 1;

    crc = Crc32Calc(raw, FLAG_CRC_OFFSET);
    if (crc != flag->crc32) return 2;

    return 0;
}

int FLASH_IF_ReadBootFlag(BootFlag_t *flag)
{
    if (flag == NULL) return 1;

    /* 主块坏了就退到备份块。返回 0 才表示"拿到一份可信的标志"。 */
    if (ReadFlagFrom(FLAG_ADDR_MAIN, flag) == 0) return 0;
    if (ReadFlagFrom(FLAG_ADDR_BACKUP, flag) == 0) return 0;

    memset(flag, 0, sizeof(*flag));
    return 2;
}
