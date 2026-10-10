#ifndef __AT24C02_H__
#define __AT24C02_H__

#include "i2c.h"


/* === AT24C02 EEPROM 参数 === */
#define AT24C02_ADDR_W        0xA0    /* 写地址：1010 000 0（A2A1A0 全接地） = 7位地址 0x50 << 1 */
#define AT24C02_ADDR_R        0xA1    /* 读地址：1010 000 1（HAL 的 Mem_Read 会自动置 R 位，实际用不到这个宏） */
#define AT24C02_PAGE_SIZE     8       /* 页大小：8 字节（页写不能跨页，跨页会回卷覆盖页首） */
#define AT24C02_TOTAL_SIZE    256     /* 总容量：256 字节（2Kbit） */
#define AT24C02_WRITE_DELAY   5       /* 写周期：5ms（每批写后必须等待，期间器件不应答） */

/* 初始化 / 探测器件是否在线
   返回：0 = 在线   1 = 无应答（没接 / 地址错 / 正在写周期）   2 = I2C1 外设尚未 MX_I2C1_Init() */
uint8_t  AT24C02_Init(void);

/* 下面的函数不做返回值检查，调用方若需要确认成功与否，
   可自行改用 HAL_I2C_Mem_Write/Read 的返回值版本 */
void     AT24C02_WriteOneByte(uint8_t addr, uint8_t data);
uint8_t  AT24C02_ReadOneByte(uint8_t addr);
void     AT24C02_WriteBytes(uint8_t addr, uint8_t *buf, uint8_t len);   /* 内部自动按 8 字节页切分 */
void     AT24C02_ReadBytes(uint8_t addr, uint8_t *buf, uint8_t len);

#endif /* __AT24C02_H__ */
