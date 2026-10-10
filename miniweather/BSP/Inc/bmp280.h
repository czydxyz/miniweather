#ifndef __BMP280_H
#define __BMP280_H

#include "main.h"
#include "i2c.h"


/* 7 位 I2C 地址（本板 SDO 接地，用 0x76） */
#define BMP280_ADDR             (0x76 << 1)   /* 8 位写地址 = 0xEC */

/* ---------- 芯片 ID ---------- */
#define BMP280_CHIP_ID          0x58    /* BMP280 */
#define BME280_CHIP_ID          0x60    /* BME280（多一个湿度通道） */

/* ---------- 寄存器地址 ---------- */
#define BMP280_REG_ID           0xD0    /* 芯片 ID */
#define BMP280_REG_RESET        0xE0    /* 软复位，写 0xB6 */
#define BMP280_REG_STATUS       0xF3    /* bit3=measuring(1=转换中) bit0=im_update(1=校准值正在搬) */
#define BMP280_REG_CTRL_MEAS    0xF4    /* bit7:5=osrs_t  bit4:2=osrs_p  bit1:0=mode */
#define BMP280_REG_CONFIG       0xF5    /* bit4:2=t_sb  bit2:0=filter */
#define BMP280_REG_PRESS_MSB    0xF7    /* 气压最高字节（0xF7/0xF8/0xF9 连续 3 字节） */
#define BMP280_REG_TEMP_MSB     0xFA    /* 温度最高字节（0xFA/0xFB/0xFC 连续 3 字节） */
#define BMP280_REG_CTRL_HUM     0xF2    /* 湿度过采样（仅 BME280 有效） */
#define BMP280_REG_HUM_MSB      0xFD    /* 湿度数据（仅 BME280，本驱动未启用） */

#define BMP280_REG_CALIB_START  0x88    /* 校准参数起始地址，共 24 字节（0x88~0x9F） */

/* 软复位命令 */
#define BMP280_RESET_VALUE      0xB6

/* ---------- 工作模式配置 ---------- */
#define BMP280_CTRL_MEAS_CFG    0x25
#define BMP280_CONFIG_CFG       0x00    /* 无滤波、待机 0.5ms（forced 模式下待机时间无意义） */

/* 转换等待时间（osrs_t×1 + osrs_p×1 时最大 6.5ms），取 7ms 保险 */
#define BMP280_MEAS_DELAY_MS    7

/* 数据结果结构 */
typedef struct
{
    float temperature;   /* 温度 ℃ */
    float pressure;      /* 气压 Pa */
    float altitude;      /* 海拔 m（根据气压推算，会随天气漂几十米，别当绝对高度用） */
} BMP280_Data_t;

/* ================== 函数声明 ================== */
uint8_t BMP280_Init(void);                  /* 初始化。0=成功 1=ID不对 2=通信失败 3=校准读失败 */
uint8_t BMP280_Read(BMP280_Data_t *data);   /* 读温度+气压+海拔。0=成功 1=通信失败 2=未初始化 3=超范围 4=空指针 */
uint8_t BMP280_ReadID(uint8_t *id);         /* 读 0xD0 的芯片 ID，用来区分 BMP280/BME280 */
uint8_t BMP280_IsBME280(void);              /* 1=BME280  0=BMP280 */
float  BMP280_CalcAltitude(float pressure); /* 气压→海拔（也可单独给 SHTC3 之外的场合用） */

#endif /* __BMP280_H */
