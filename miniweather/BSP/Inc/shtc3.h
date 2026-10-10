#ifndef __SHTC3_H
#define __SHTC3_H

#include "main.h"
#include "i2c.h"


/* 7 位 I2C 地址，左移 1 位变成 HAL 要的 8 位地址 */
#define SHTC3_ADDR              (0x70 << 1)   /* = 0xE0 */

/* ---------- 命令字（手册 Table 11，16 位，高字节先发）---------- */
/* 命令是 2×2 矩阵：模式(Normal/LowPower) × 时钟拉伸(开/关) × 谁先发(T/RH)
   本项目用 Normal + 拉伸开 + 温度优先 = 0x7CA2 */
#define SHTC3_CMD_MEAS_NORMAL     0x7CA2   /* Normal + 拉伸开 + T 优先 ★本项目用这个 */
#define SHTC3_CMD_MEAS_LP         0x6458   /* LowPower + 拉伸开 + T 优先 */
#define SHTC3_CMD_MEAS_N_CLKDIS   0x7866   /* Normal + 拉伸关 + T 优先（用这个必须自己 delay 15ms）*/
#define SHTC3_CMD_MEAS_LP_CLKDIS  0x609C   /* LowPower + 拉伸关 + T 优先 */

#define SHTC3_CMD_WAKEUP          0x3517   /* 唤醒（休眠后必须先唤醒） */
#define SHTC3_CMD_SLEEP           0xB098   /* 进入休眠 */
#define SHTC3_CMD_SOFT_RESET      0x805D   /* 软复位 */
#define SHTC3_CMD_READ_ID         0xEFC8   /* 读 ID（返回 3 字节：ID(2)+CRC(1)） */

/* ---------- 超时（ms）---------- */
#define SHTC3_TIMEOUT             100     /* 必须 > 12.1ms，否则时钟拉伸会被判超时 */

/* 温湿度数据结构 */
typedef struct
{
    float temperature;   /* 温度 ℃ */
    float humidity;      /* 湿度 %RH */
} SHTC3_Data_t;

/* ================== 函数声明 ================== */
uint8_t SHTC3_Init(void);                       /* 初始化：软复位+唤醒。0=成功 1=复位失败 2=唤醒失败 */
uint8_t SHTC3_Read(SHTC3_Data_t *data);         /* 读温湿度。0=成功 1=通信失败 2=CRC错 3=超范围 4=空指针 */
uint8_t SHTC3_ReadID(uint16_t *id);             /* 读芯片 ID（自检用）。0=成功 1=命令失败 2=读失败 3=CRC错 */

#endif /* __SHTC3_H */
