#ifndef __SGP30_H
#define __SGP30_H

#include "main.h"
#include "i2c.h"


/* 7 位 I2C 地址 */
#define SGP30_ADDR                  (0x58 << 1)   /* 8 位写地址 = 0xB0 */

/* ---------- 命令字（手册 Table 10，16 位，高字节先发）---------- */
#define SGP30_CMD_INIT_AIR_QUALITY      0x2003   /* 初始化空气质量测量（执行 max 10ms） */
#define SGP30_CMD_MEASURE_AIR_QUALITY   0x2008   /* 测量（返回 6 字节，执行 max 12ms）★每 1 秒一次 */
#define SGP30_CMD_GET_BASELINE          0x2015   /* 读基线（返回 6 字节） */
#define SGP30_CMD_SET_BASELINE          0x201E   /* 写基线（参数 6 字节） */
#define SGP30_CMD_SET_HUMIDITY          0x2061   /* 设置绝对湿度（参数 3 字节） */
#define SGP30_CMD_MEASURE_TEST          0x2032   /* 自检（返回 3 字节，执行 max 220ms） */
#define SGP30_CMD_GET_FEATURE_SET       0x202F   /* 读特性集版本（返回 3 字节） */
#define SGP30_CMD_MEASURE_RAW           0x2050   /* 读原始信号（返回 6 字节） */
#define SGP30_CMD_GET_SERIAL_ID         0x3682   /* 读 48 位序列号（返回 9 字节） */

/* ---------- 时序（毫秒，都按手册 max 取）---------- */
#define SGP30_DELAY_INIT_MS             10      /* 0x2003 后 */
#define SGP30_DELAY_MEASURE_MS          12      /* 0x2008 后 ★这个不能少 */
#define SGP30_DELAY_BASELINE_MS         10      /* 基线命令后 */
#define SGP30_DELAY_TEST_MS             220     /* 自检命令后（最慢的一个） */
#define SGP30_DELAY_ID_MS               10      /* 读序列号后 */

/* ---------- 自检通过的期望值 ---------- */
#define SGP30_TEST_PASS_VALUE           0xD400  /* Measure_test 返回 0xD400 表示通过 */

/* ---------- 湿度补偿默认值 ---------- */
/* 不写 Set_humidity 时，传感器内部默认按 0x0B92(11.57 g/m³) 补偿，
   不是"不补偿"。想关闭补偿要显式写 0x0000。 */
#define SGP30_HUMIDITY_DEFAULT          0x0B92

/* 数据结构 */
typedef struct
{
    uint16_t co2;    /* CO₂ 当量 ppm（400~60000，正常大气约 400） */
    uint16_t tvoc;   /* TVOC ppb（0~60000） */
} SGP30_Data_t;

/* ================== 函数声明 ================== */
uint8_t SGP30_Init(void);                                    /* 初始化：发 0x2003。0=成功 */
uint8_t SGP30_Read(SGP30_Data_t *data);                      /* 读 CO2eq+TVOC。0=成功 1=通信失败 2=CRC错 3=超范围 4=空指针 */

uint8_t SGP30_GetSerialID(uint8_t id[6]);                    /* 读 48 位序列号（9 字节里抽 6 个有效字节） */
uint8_t SGP30_MeasureTest(void);                             /* 自检。0=通过 1=失败 2=通信失败 */
uint8_t SGP30_GetFeatureSet(uint16_t *version);              /* 读特性集版本 */

uint8_t SGP30_GetBaseline(uint16_t *co2_base, uint16_t *tvoc_base);   /* 读基线：返回顺序 (CO2eq, TVOC) */
uint8_t SGP30_SetBaseline(uint16_t tvoc_base, uint16_t co2_base);     /* 写基线：参数顺序 (TVOC, CO2eq) ★反的！ */
uint8_t SGP30_SetAbsoluteHumidity(float humidity_rh, float temp_c);   /* 用相对湿度+温度算绝对湿度并写入 */
uint16_t SGP30_CalcAbsoluteHumidity(float humidity_rh, float temp_c); /* 只算不写，返回 8.8 定点值 */

#endif /* __SGP30_H */
