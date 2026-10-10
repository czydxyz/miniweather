/* JEDEC ID：0xEF4018=Winbond W25Q128 / 0x684018=Boya BY25Q128（板载实测为此片）
 *          0xC84018=GigaDevice GD25Q128；三家「类型+容量」均为 0x4018，仅厂商字节不同。
 */
#ifndef __W25Q128_H
#define __W25Q128_H

#include "main.h"
#include "spi.h"


/* 片选操作（直接用 main.h 的 FLASH_CS 宏） */
#define W25Q_CS_L()  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET)
#define W25Q_CS_H()  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET)

/* ================== 指令表 ================== */
#define W25Q_WRITE_ENABLE   0x06   /* 写使能 */
#define W25Q_WRITE_DISABLE  0x04   /* 写禁止 */
#define W25Q_READ_STATUS1   0x05   /* 读状态寄存器 1 */
#define W25Q_READ_STATUS2   0x35   /* 读状态寄存器 2 */
#define W25Q_READ_DATA      0x03   /* 读数据（标准） */
#define W25Q_FAST_READ      0x0B   /* 快速读 */
#define W25Q_PAGE_PROGRAM   0x02   /* 页写（一页最多 256 字节） */
#define W25Q_SECTOR_ERASE   0x20   /* 扇区擦除（4KB） */
#define W25Q_BLOCK_ERASE_32K 0x52  /* 块擦除（32KB） */
#define W25Q_BLOCK_ERASE_64K 0xD8  /* 块擦除（64KB） */
#define W25Q_CHIP_ERASE     0xC7   /* 整片擦除（慎用！） */
#define W25Q_POWER_DOWN     0xB9   /* 掉电 */
#define W25Q_RELEASE_POWER_DOWN 0xAB /* 唤醒 */
#define W25Q_DEVICE_ID      0x90   /* 读设备 ID */
#define W25Q_JEDEC_ID       0x9F   /* 读 JEDEC ID（最常用，返回 3 字节） */

/* ================== 容量参数 ================== */
#define W25Q128_TOTAL_SIZE   16777216UL   /* 16MB 总字节数 */
#define W25Q128_PAGE_SIZE    256          /* 页大小 256 字节 */
#define W25Q128_SECTOR_SIZE  4096         /* 扇区大小 4KB */
#define W25Q128_SECTOR_COUNT 4096         /* 扇区总数 4096 个 */

#define W25Q_MID_WINBOND     0xEF   /* Winbond 华邦 */
#define W25Q_MID_BOYA        0x68   /* Boya 比亚迪（本项目实测为此片） */
#define W25Q_MID_GIGADEVICE  0xC8   /* GigaDevice 兆易创新 */
#define W25Q_TYPE_CAP_16MB   0x4018 /* 存储类型+容量：0x4018 = 128M-bit = 16MB */

/* 判定一个 JEDEC ID 是否属于 16MB 的 25Q 系列 */
#define W25Q128_ID_IS_VALID(id)                                   \
    ( (((id) & 0x00FFFFu) == W25Q_TYPE_CAP_16MB) &&              \
      ((((id) >> 16) & 0xFFu) != 0x00u) &&                       \
      ((((id) >> 16) & 0xFFu) != 0xFFu) )

/* ================== 函数声明 ================== */
void        W25Q128_Init(void);                      /* 初始化：CS 拉高 + 唤醒 */
uint32_t    W25Q128_ReadID(void);                    /* 读 JEDEC ID（3 字节拼成 24 位） */
uint8_t     W25Q128_IDIsValid(uint32_t id);          /* 该 ID 是否为 16MB 的 25Q 系列 */
const char *W25Q128_IDName(uint32_t id);             /* 返回厂商字符串，如 "BY25Q128" */
uint8_t  W25Q128_Read(uint32_t addr, uint8_t *buf, uint32_t len);  /* 读数据 */
uint8_t  W25Q128_Write(uint32_t addr, uint8_t *buf, uint32_t len);  /* 写数据（自动跨页） */
uint8_t  W25Q128_EraseSector(uint32_t addr);      /* 擦除一个扇区（4KB） */
uint8_t  W25Q128_EraseChip(void);                 /* 整片擦除（慎用） */
void     W25Q128_PowerDown(void);                 /* 掉电（低功耗） */
void     W25Q128_WakeUp(void);                    /* 唤醒 */

#endif /* __W25Q128_H */
