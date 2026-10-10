#include "crc32.h"
#include "boot_cfg.h"

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

uint32_t CRC32_Calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;

    for (i = 0; i < len; i++) crc = Crc32Step(crc, data[i]);
    return crc ^ 0xFFFFFFFFu;
}

/* 直接对 Flash 里的内容算 CRC32。
   为什么要单独写一个：固件有 256KB，不可能先搬到 RAM（总共才 192KB）。
   好在 STM32 是 XIP，Flash 能当内存读，指针扫一遍就行。 */
uint32_t CRC32_CalcFlash(uint32_t flash_addr, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;

    for (i = 0; i < len; i++) crc = Crc32Step(crc, FLASH_PTR8(flash_addr)[i]);
    return crc ^ 0xFFFFFFFFu;
}
