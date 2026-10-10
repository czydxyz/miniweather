/* W25Q128 SPI Flash 驱动（SPI1：PB3/SCK PB4/MISO PB5/MOSI，CS=PB14）
 * [约束] 写前必须擦除（最小单位 4KB 扇区）；页写最大 256B 且不跨页，跨页回卷本页开头。
 * [约束] 每条擦/写后必须等 BUSY 清零：扇区擦典型 45ms / 最大 400ms，整片擦最长 200s。
 * [约束] 与 LCD 共用 SPI1 总线，片选必须互斥，空闲一律拉高。
 */
#include "w25q128.h"
#include "delay.h"

/*
 * 底层：SPI1 收发一个字节
 * SPI 是全双工：主机发一个字节的同时必然收到一个字节。
 * 读操作时"发什么不重要"（一般发 0xFF），重要的是收到的那一字节。
  */
static uint8_t W25Q_SPI_Byte(uint8_t tx)
{
    uint8_t rx = 0xFF;

    /* 超时 100ms：SPI 一次字节传输只要几微秒，100ms 已经非常宽松 */
    HAL_SPI_TransmitReceive(&hspi1, &tx, &rx, 1, 100);
    return rx;
}

/*
 * 读状态寄存器 1
 *   bit0 BUSY = 1 表示芯片正在擦/写，此时除了读状态寄存器，其它命令全被忽略
 *   bit1 WEL  = 1 表示允许写入（发过 0x06 之后才是 1）
  */
static uint8_t W25Q_ReadStatus(void)
{
    uint8_t st;

    W25Q_CS_L();
    W25Q_SPI_Byte(W25Q_READ_STATUS1);
    st = W25Q_SPI_Byte(0xFF);        /* 发 0xFF 把数据"顶"出来 */
    W25Q_CS_H();
    return st;
}

static uint8_t W25Q_WaitBusy(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();

    while (HAL_GetTick() - t0 < timeout_ms) {
        if ((W25Q_ReadStatus() & 0x01u) == 0u) return 0;   /* BUSY 位清了 */
        delay_ms(1);
    }
    return 1;
}

/*
 * 写使能（0x06）
 * 每一次擦/写之前都要发，发完芯片会在操作结束后自动清掉
  */
static void W25Q_WriteEnable(void)
{
    W25Q_CS_L();
    W25Q_SPI_Byte(W25Q_WRITE_ENABLE);
    W25Q_CS_H();
}

/*
 * 一、初始化
 * 只需要把片选拉高（空闲态），并发一条"唤醒"命令把芯片从掉电模式拉回来
  */
void W25Q128_Init(void)
{
    W25Q_CS_H();                     /* 空闲时片选必须为高，否则芯片一直被选中 */
    delay_ms(1);
    W25Q128_WakeUp();                /* 0xAB 唤醒（上电默认是正常模式，发了也无害） */
    delay_ms(1);
}

/*
 * 二、读 JEDEC ID（0x9F）
 *   返回 3 字节：厂商 ID 0xEF(华邦) + 存储类型 0x40 + 容量 0x18(16MB)
 *   拼成 0xEF4018。读不到这个值 = 接线错 / 片选没拉 / SPI 模式不对。
 *   ★ 这是判断 W25Q128 是否正常的第一手段，务必在自检里先跑它
  */
uint32_t W25Q128_ReadID(void)
{
    uint32_t id = 0;

    W25Q_CS_L();
    W25Q_SPI_Byte(W25Q_JEDEC_ID);
    id  = ((uint32_t)W25Q_SPI_Byte(0xFF)) << 16;
    id |= ((uint32_t)W25Q_SPI_Byte(0xFF)) << 8;
    id |= ((uint32_t)W25Q_SPI_Byte(0xFF));
    W25Q_CS_H();

    return id;
}

/*
 * 二点五、ID 判定与厂商名
 *   为什么不能死磕 0xEF4018：同一块板可能贴不同厂的兼容片，
 *   华邦 0xEF / 比亚迪 0x68 / 兆易 0xC8，容量字节都是 0x4018。
 *   只认 0xEF 会把良品判成 FAIL（本项目实测片就是 BY25Q128 → 0x684018）。
  */
uint8_t W25Q128_IDIsValid(uint32_t id)
{
    return W25Q128_ID_IS_VALID(id) ? 1u : 0u;
}

const char *W25Q128_IDName(uint32_t id)
{
    uint8_t mid = (uint8_t)((id >> 16) & 0xFFu);

    switch (mid) {
        case W25Q_MID_WINBOND:    return "W25Q128";    /* 华邦 */
        case W25Q_MID_BOYA:       return "BY25Q128";   /* 比亚迪 */
        case W25Q_MID_GIGADEVICE: return "GD25Q128";   /* 兆易创新 */
        case 0x00:
        case 0xFF:                return "no-chip";    /* 没接 / 片选没拉对 */
        default:                  return "unknown-25Q";/* 别的兼容片，容量对了就能用 */
    }
}

/*
 * 三、读数据（0x03）
 *   发完命令 + 3 字节地址后，芯片会连续吐数据，地址自动加一，
 *   可以一直读到芯片末尾（不会像 EEPROM 那样按页回卷）。
 *   所以这里不需要分页处理，直接读 len 个字节即可。
  */
uint8_t W25Q128_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint32_t i;

    if (buf == NULL || len == 0) return 1;
    if (addr >= W25Q128_TOTAL_SIZE) return 1;

    if (W25Q_WaitBusy(500)) return 2;              /* 上一次擦写还没结束 */

    W25Q_CS_L();
    W25Q_SPI_Byte(W25Q_READ_DATA);
    W25Q_SPI_Byte((uint8_t)(addr >> 16));          /* 地址高字节先发（大端） */
    W25Q_SPI_Byte((uint8_t)(addr >> 8));
    W25Q_SPI_Byte((uint8_t)(addr));
    for (i = 0; i < len; i++) {
        buf[i] = W25Q_SPI_Byte(0xFF);
    }
    W25Q_CS_H();

    return 0;
}

static uint8_t W25Q_WritePage(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    W25Q_WriteEnable();

    W25Q_CS_L();
    W25Q_SPI_Byte(W25Q_PAGE_PROGRAM);
    W25Q_SPI_Byte((uint8_t)(addr >> 16));
    W25Q_SPI_Byte((uint8_t)(addr >> 8));
    W25Q_SPI_Byte((uint8_t)(addr));
    for (i = 0; i < len; i++) {
        W25Q_SPI_Byte(buf[i]);
    }
    W25Q_CS_H();

    return W25Q_WaitBusy(50);        /* 页写典型 0.7ms，给 50ms 足够 */
}

/*
 * 五、写数据（自动跨页）
 *   计算页面剩余空间，把数据切成若干批，每批不超过一页末尾。
 *   ⚠️ 调用前必须保证目标区域已经被擦除（全 0xFF），否则写不进去
  */
uint8_t W25Q128_Write(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint32_t i = 0;
    uint16_t page_left, chunk;

    if (buf == NULL || len == 0) return 1;
    if (addr >= W25Q128_TOTAL_SIZE) return 1;
    if (addr + len > W25Q128_TOTAL_SIZE) return 1;   /* 越界保护 */

    while (i < len) {
        /* 本页还剩多少字节（页面大小 256，页内偏移 = addr & 0xFF） */
        page_left = (uint16_t)(W25Q128_PAGE_SIZE -
                    (uint16_t)((addr + i) & (W25Q128_PAGE_SIZE - 1u)));
        chunk = (uint16_t)((len - i) < page_left ? (len - i) : page_left);

        if (W25Q_WritePage(addr + i, buf + i, chunk) != 0) return 2;
        i += chunk;
    }
    return 0;
}

/*
 * 六、扇区擦除（0x20，4KB）
 *   地址不必非得是扇区开头 —— 芯片会自动对齐到该地址所在的扇区，
 *   也就是说"擦 0x00001234"实际擦的是 0x00001000~0x00001FFF 这 4KB。
 *   ★ 所以传地址时请确认：你要擦的扇区里没有别的数据
  */
uint8_t W25Q128_EraseSector(uint32_t addr)
{
    if (addr >= W25Q128_TOTAL_SIZE) return 1;

    if (W25Q_WaitBusy(500)) return 2;

    W25Q_WriteEnable();

    W25Q_CS_L();
    W25Q_SPI_Byte(W25Q_SECTOR_ERASE);
    W25Q_SPI_Byte((uint8_t)(addr >> 16));
    W25Q_SPI_Byte((uint8_t)(addr >> 8));
    W25Q_SPI_Byte((uint8_t)(addr));
    W25Q_CS_H();

    return W25Q_WaitBusy(1000);      /* 扇区擦除典型 45ms，最大 400ms */
}

/*
 * 七、整片擦除（0xC7）
 *   ⚠️ 最长要 200 秒，会把 16MB 全清成 0xFF。
 *      本项目里没有任何地方需要它，只留给"恢复出厂"用。
  */
uint8_t W25Q128_EraseChip(void)
{
    if (W25Q_WaitBusy(500)) return 2;

    W25Q_WriteEnable();

    W25Q_CS_L();
    W25Q_SPI_Byte(W25Q_CHIP_ERASE);
    W25Q_CS_H();

    return W25Q_WaitBusy(200000);    /* 200 秒上限 */
}

/* 掉电（0xB9）：电流降到 1µA，唤醒前所有命令无效 */
void W25Q128_PowerDown(void)
{
    W25Q_CS_L();
    W25Q_SPI_Byte(W25Q_POWER_DOWN);
    W25Q_CS_H();
    delay_us(3);                     /* tDP：片选拉高后至少 3µs 才进掉电 */
}

/* 唤醒（0xAB）：发完命令后要等 tRES1(3µs) 芯片才恢复 */
void W25Q128_WakeUp(void)
{
    W25Q_CS_L();
    W25Q_SPI_Byte(W25Q_RELEASE_POWER_DOWN);
    W25Q_CS_H();
    delay_us(3);
}
