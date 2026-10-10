/* AT24C02 EEPROM 驱动（硬件 I2C1，板载，地址 0x50）
 * [约束] AT24C02_Init() 会把 PB8/PB9 从 AF4_I2C1 改回 GPIO 开漏输出 → 硬件 I2C 断开，
 *        SHTC3/BMP280/SGP30 全部超时。禁止调用，也禁止在别处重配 PB8/PB9。
 * [约束] 页写 8 字节且不跨页：跨页会回卷到本页开头覆盖已有数据，须按页边界分批。
 * [约束] 每页写后必须延时 >=5ms（写周期），不可删。
 */
#include "at24c02.h"
#include "delay.h"


uint8_t AT24C02_Init(void)
{
    /* 先确认 I2C1 外设已经 MX_I2C1_Init() 过。
       State 在 HAL_I2C_Init 之后是 READY，没初始化是 RESET。
       区分这两种情况，排错时能省很多时间。 */
    if (hi2c1.State != HAL_I2C_STATE_READY)
        return 2;

    /* IsDeviceReady 内部会发地址+写方向并等待 ACK：
       Trials=3 表示最多探测 3 次，每次最多等 100ms。
       上电后若器件正处在内部写周期会暂时不回应，重试机制正好覆盖。 */
    if (HAL_I2C_IsDeviceReady(&hi2c1, AT24C02_ADDR_W, 3, 100) == HAL_OK)
        return 0;

    return 1;
}


void AT24C02_WriteOneByte(uint8_t addr, uint8_t data)
{
    HAL_I2C_Mem_Write(&hi2c1,
                      AT24C02_ADDR_W,        /* 0xA0 = 7 位地址 0x50 左移 1 位，正好可直接用 */
                      addr,                  /* EEPROM 内部存储地址 */
                      I2C_MEMADD_SIZE_8BIT,  /* AT24C02 地址是 8 位（24C32 以上才是 16 位） */
                      &data,
                      1,
                      100);                  /* 超时 100ms */

    delay_ms(AT24C02_WRITE_DELAY);
}


/* 从指定地址读一个字节

   HAL_I2C_Mem_Read 自动展开成「伪写 + 重复起始」：
       START -> 设备地址(W) -> 存储地址 -> RESTART -> 设备地址(R) -> 读数据(NACK) -> STOP
   这正是软件 I2C 版手写 iic_start/iic_sendbyte 那 10 行的等价物。
 */
uint8_t AT24C02_ReadOneByte(uint8_t addr)
{
    uint8_t data = 0;

    HAL_I2C_Mem_Read(&hi2c1, AT24C02_ADDR_W, addr, I2C_MEMADD_SIZE_8BIT, &data, 1, 100);

    return data;
}


void AT24C02_WriteBytes(uint8_t addr, uint8_t *buf, uint8_t len)
{
    uint8_t sent = 0;      /* 已写入字节数 */
    uint8_t cur  = addr;   /* 当前写到哪个地址 */

    if (buf == 0 || len == 0)
        return;

    /* 不做容量截断：len 是 uint8_t，最大 255，本来就不会超过 256 字节。
       注意 addr + len 超过 256 时，AT24C02 的地址计数器会绕回 0x00，
       这是器件本身的行为（顺序读/页写都会回卷），调用方自己保证不越界。 */

    while (sent < len)
    {
        uint8_t page_off;  /* 当前地址在本页内的偏移 0~7 */
        uint8_t room;      /* 本页还剩几个字节可写 */
        uint8_t n;         /* 这一批实际写几个字节 */

        page_off = (uint8_t)(cur & (AT24C02_PAGE_SIZE - 1));  /* PAGE_SIZE=8 是 2 的幂，&7 等价于 %8 */
        room     = (uint8_t)(AT24C02_PAGE_SIZE - page_off);

        n = (uint8_t)(len - sent);
        if (n > room)
            n = room;                                          /* 本批不跨页 */

        if (HAL_I2C_Mem_Write(&hi2c1, AT24C02_ADDR_W, cur,
                              I2C_MEMADD_SIZE_8BIT, &buf[sent], n, 100) != HAL_OK)
        {
            return;    /* 写入失败（器件无应答 / 总线忙），直接退出 */
        }

        delay_ms(AT24C02_WRITE_DELAY);   /* 每一批都要等写周期，不能只在最后等一次 */

        cur  = (uint8_t)(cur + n);
        sent = (uint8_t)(sent + n);
    }
}


/* 连续读取多个字节

   顺序读不受页限制——地址计数器会自动跨页递增，
   所以一次 Mem_Read 读满 256 字节也没问题，不用像写那样分批。
 */
void AT24C02_ReadBytes(uint8_t addr, uint8_t *buf, uint8_t len)
{
    if (buf == 0 || len == 0)
        return;

    HAL_I2C_Mem_Read(&hi2c1, AT24C02_ADDR_W, addr, I2C_MEMADD_SIZE_8BIT, buf, len, 100);
}
