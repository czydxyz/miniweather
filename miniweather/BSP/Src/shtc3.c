/* SHTC3 温湿度传感器驱动（I2C 地址 0x70，HAL 传 0x70<<1）
 * 命令 0x7CA2（Normal + 时钟拉伸开 + 温度优先）；读 6 字节：T(2)+CRC(1) 与 RH(2)+CRC(1)
 * 换算：T = -45 + 175*raw/65536；RH = 100*raw/65536
 * CRC-8：poly 0x31，init 0xFF，不反射；自检 CRC(0x00) = 0xAC
 * [约束] 调用方必须检查返回值，否则会拿旧数据或乱码当真值。
 */
#include "shtc3.h"
#include "delay.h"

/* SHTC3 传感器句柄使用 CubeMX 生成的 hi2c1（在 i2c.c 里定义） */
extern I2C_HandleTypeDef hi2c1;


/**
 * @brief  CRC-8 校验（Sensirion 通用参数）
 * @param  data 数据首地址
 * @param  len  数据长度（SHTC3/SGP30 都是每次校验 2 字节）
 * @retval CRC 值
 * @note   算法要点：每字节先异或进 crc，然后左移 8 次，
 *         移出的位为 1 时异或多项式 0x31。
 *         【不要】用反射版（>>1 + poly 0x8C），那个算出来全错。
 */
static uint8_t SHTC3_CRC8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0xFF;                 /* 初值 0xFF */
    uint8_t i, j;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];                 /* 先把字节异或进来 */
        for (j = 0; j < 8; j++)
        {
            if (crc & 0x80)             /* 最高位为 1（左移版的关键判断） */
                crc = (uint8_t)((crc << 1) ^ 0x31);
            else
                crc = (uint8_t)(crc << 1);
        }
    }
    return crc;                         /* 无 FinalXOR */
}

/**
 * @brief  发送 2 字节命令
 * @param  cmd 16 位命令字
 * @retval 0=成功  1=I2C 通信失败（没收到 ACK）
 * @note   一定要返回状态！返回 void 会把"传感器掉了/线松了"吞掉，
 *         上层就会拿着上一次的旧数据当新数据显示。
 */
static uint8_t SHTC3_SendCmd(uint16_t cmd)
{
    uint8_t buf[2];

    buf[0] = (uint8_t)(cmd >> 8);       /* 高字节先发（大端） */
    buf[1] = (uint8_t)(cmd & 0xFF);

    if (HAL_I2C_Master_Transmit(&hi2c1, SHTC3_ADDR, buf, 2, 100) != HAL_OK)
        return 1;

    return 0;
}

/**
 * @brief  SHTC3 初始化：软复位 → 唤醒
 * @retval 0=成功  1=复位命令失败  2=唤醒命令失败
 */
uint8_t SHTC3_Init(void)
{
    /* ① 软复位 0x805D：让传感器回到确定的初始状态 */
    if (SHTC3_SendCmd(SHTC3_CMD_SOFT_RESET) != 0)
        return 1;

    delay_ms(1);                        /* 复位后需等 tSR（最大 240us），1ms 足够 */

    /* ② 唤醒 0x3517：退出睡眠后才能接收命令 */
    if (SHTC3_SendCmd(SHTC3_CMD_WAKEUP) != 0)
        return 2;

    delay_ms(1);                        /* tPU（最大 240us）*/

    return 0;
}

/**
 * @brief  读芯片 ID（自检用，接好线先读这个确认通信正常）
 * @param  id  返回 16 位 ID 值
 * @retval 0=成功  1=命令失败  2=读取失败  3=CRC 错误
 * @note   命令 0xEFC8，返回 3 字节：ID(2) + CRC(1)
 *         SHTC3 的 ID 里 bit11~bit8 固定为 0x8，所以常见值是 0x0807 左右
 */
uint8_t SHTC3_ReadID(uint16_t *id)
{
    uint8_t buf[3];

    if (SHTC3_SendCmd(SHTC3_CMD_WAKEUP) != 0) return 1;
    delay_ms(1);

    if (SHTC3_SendCmd(SHTC3_CMD_READ_ID) != 0) return 1;

    if (HAL_I2C_Master_Receive(&hi2c1, SHTC3_ADDR, buf, 3, 100) != HAL_OK)
        return 2;

    if (SHTC3_CRC8(&buf[0], 2) != buf[2]) return 3;   /* CRC 不过 → 数据不可信 */

    *id = (uint16_t)((buf[0] << 8) | buf[1]);

    SHTC3_SendCmd(SHTC3_CMD_SLEEP);                   /* 用完睡下 */
    return 0;
}

/**
 * @brief  读温度和湿度
 * @param  data 结果结构体（temperature=℃、humidity=%RH）
 * @retval 0=成功
 *         1=命令/通信失败
 *         2=CRC 校验失败（数据被丢弃，不会写入 data）
 *         3=数据超出合理范围（兜底检查）
 *         4=参数为空指针
 * @note   单次耗时约 14ms（唤醒+测量），在 FreeRTOS 任务里调用没问题，
 *         但别在中断里调。
 */
uint8_t SHTC3_Read(SHTC3_Data_t *data)
{
    uint8_t  buf[6];
    uint16_t raw_t, raw_h;
    float    t, rh;

    if (data == NULL) return 4;

    /* ① 唤醒 */
    if (SHTC3_SendCmd(SHTC3_CMD_WAKEUP) != 0) return 1;
    delay_ms(1);

    /* ② 发测量命令 0x7CA2（Normal + 时钟拉伸开 + 温度优先）
         温度优先 ⇒ buf[0..1] 是温度，buf[3..4] 是湿度 */
    if (SHTC3_SendCmd(SHTC3_CMD_MEAS_NORMAL) != 0) return 1;

    /* ③ 等转换完成（Normal 最大 12.1ms）
         用 0x7CA2 时传感器会拉住 SCL 自动等，这行是冗余保险，可以删 */
    delay_ms(13);

    /* ④ 读 6 字节 */
    if (HAL_I2C_Master_Receive(&hi2c1, SHTC3_ADDR, buf, 6, 100) != HAL_OK)
        return 1;

    /* ⑤ CRC 校验：温度(2字节)+CRC、湿度(2字节)+CRC，各自独立校验 */
    if (SHTC3_CRC8(&buf[0], 2) != buf[2]) return 2;   /* 温度 CRC */
    if (SHTC3_CRC8(&buf[3], 2) != buf[5]) return 2;   /* 湿度 CRC */

    /* ⑥ 换算（分母是 65536 不是 65535！） */
    raw_t = (uint16_t)((buf[0] << 8) | buf[1]);
    raw_h = (uint16_t)((buf[3] << 8) | buf[4]);

    t  = -45.0f + 175.0f * (float)raw_t / 65536.0f;
    rh = 100.0f * (float)raw_h / 65536.0f;

    /* ⑦ 范围检查：SHTC3 没有 CRC 之外的保护，
         偶尔的位翻转可能凑巧通过 CRC（概率 1/256），
         所以再用物理量范围兜一道底 */
    if (t < -40.0f || t > 125.0f)     return 3;       /* 工作范围 -40~125℃ */
    if (rh < 0.0f   || rh > 100.0f)   return 3;

    /* ⑧ 数据没问题才写入 */
    data->temperature = t;
    data->humidity    = rh;

    /* ⑨ 用完进休眠省电（下次读之前会先唤醒） */
    SHTC3_SendCmd(SHTC3_CMD_SLEEP);

    return 0;
}
