/* BMP280 / BME280 驱动（I2C 地址 0x76）
 * 芯片 ID：0x58 = BMP280，0x60 = BME280（丝印无法区分，两个都收）
 * 校准参数 0x88~0x9F 共 24 字节；数据 20 位需 >>4；ctrl_meas = 0x25
 * [约束] 气压补偿计算必须用 int64_t，32 位会溢出。
 */
#include "bmp280.h"
#include "delay.h"
#include <math.h>

/* BMP280 传感器句柄使用 CubeMX 生成的 hi2c1 */
extern I2C_HandleTypeDef hi2c1;


/* 校准系数（Init 时读一次，之后一直用） */
static uint16_t dig_T1, dig_P1;
static int16_t  dig_T2, dig_T3;
static int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

/* t_fine：温度补偿的中间量，气压公式必须用它。
   它的物理意义是 【5120 × 摄氏度】，相当于把温度放大 5120 倍存成整数，
   这样气压公式就能用整数运算拿到足够精度。必须全局保存。 */
static int32_t  t_fine = 0;

/* 初始化完成标志 + 芯片型号 */
static uint8_t  s_init_ok   = 0;
static uint8_t  s_is_bme280 = 0;

/**
 * @brief  读芯片 ID
 * @param  id  返回的 ID（0x58=BMP280，0x60=BME280）
 * @retval 0=成功  1=通信失败
 */
uint8_t BMP280_ReadID(uint8_t *id)
{
    if (HAL_I2C_Mem_Read(&hi2c1, BMP280_ADDR, BMP280_REG_ID,
                         1, id, 1, 100) != HAL_OK)
        return 1;
    return 0;
}

/** @brief  1=BME280  0=BMP280 */
uint8_t BMP280_IsBME280(void)
{
    return s_is_bme280;
}

/**
 * @brief  BMP280 初始化
 * @retval 0=成功
 *         1=芯片 ID 不对（可能没接好，或者地址不是 0x76）
 *         2=I2C 通信失败
 *         3=校准系数读取失败
 */
uint8_t BMP280_Init(void)
{
    uint8_t id = 0;
    uint8_t calib[24];
    uint8_t cfg;

    s_init_ok = 0;

    /* ① 读芯片 ID，判断是 BMP280 还是 BME280（两种都接受） */
    if (BMP280_ReadID(&id) != 0)
        return 2;                                   /* 连 ID 都读不到 = 通信失败 */

    if (id == BME280_CHIP_ID)
        s_is_bme280 = 1;
    else if (id == BMP280_CHIP_ID)
        s_is_bme280 = 0;
    else
        return 1;                                   /* 芯片不对 */

    /* ② 软复位：写 0xE0 = 0xB6。等 2ms 让内部振荡器稳定 */
    cfg = BMP280_RESET_VALUE;
    if (HAL_I2C_Mem_Write(&hi2c1, BMP280_ADDR, BMP280_REG_RESET,
                          1, &cfg, 1, 100) != HAL_OK)
        return 2;
    delay_ms(2);

    /* ③ 读 24 字节校准系数（0x88~0x9F，芯片内地址自增，一次读完）
         注意：0x88~0x9F 是 24 字节，不是 26 字节！
         （网上很多例程写 0x88~0xA1 是错的，那是把 BME280 湿度的 dig_H1 也算进来了）*/
    if (HAL_I2C_Mem_Read(&hi2c1, BMP280_ADDR, BMP280_REG_CALIB_START,
                         1, calib, 24, 100) != HAL_OK)
        return 3;

    /* ④ 拆解校准系数：小端格式（低字节在前）
         ⚠️ dig_T1 / dig_P1 是 uint16，其余是 int16
         字节序：(calib[1]<<8)|calib[0]、((calib[3]<<8)|calib[2])、... */
    dig_T1 = (uint16_t)(((uint16_t)calib[1]  << 8) | calib[0]);
    dig_T2 = (int16_t) (((uint16_t)calib[3]  << 8) | calib[2]);
    dig_T3 = (int16_t) (((uint16_t)calib[5]  << 8) | calib[4]);

    dig_P1 = (uint16_t)(((uint16_t)calib[7]  << 8) | calib[6]);
    dig_P2 = (int16_t) (((uint16_t)calib[9]  << 8) | calib[8]);
    dig_P3 = (int16_t) (((uint16_t)calib[11] << 8) | calib[10]);
    dig_P4 = (int16_t) (((uint16_t)calib[13] << 8) | calib[12]);
    dig_P5 = (int16_t) (((uint16_t)calib[15] << 8) | calib[14]);
    dig_P6 = (int16_t) (((uint16_t)calib[17] << 8) | calib[16]);
    dig_P7 = (int16_t) (((uint16_t)calib[19] << 8) | calib[18]);
    dig_P8 = (int16_t) (((uint16_t)calib[21] << 8) | calib[20]);
    dig_P9 = (int16_t) (((uint16_t)calib[23] << 8) | calib[22]);

    /* ⑤ 配置：BME280 先把湿度关掉（我们用 SHTC3 的湿度，更准） */
    if (s_is_bme280)
    {
        cfg = 0x00;                                 /* osrs_h = 000 = 跳过湿度测量 */
        HAL_I2C_Mem_Write(&hi2c1, BMP280_ADDR, BMP280_REG_CTRL_HUM,
                          1, &cfg, 1, 100);
    }

    /* ⑥ 配置 ctrl_meas = 0x25（温度×1 / 气压×1 / forced）
         以及 config = 0x00（无 IIR 滤波）*/
    cfg = BMP280_CONFIG_CFG;
    HAL_I2C_Mem_Write(&hi2c1, BMP280_ADDR, BMP280_REG_CONFIG,
                      1, &cfg, 1, 100);

    cfg = BMP280_CTRL_MEAS_CFG;
    if (HAL_I2C_Mem_Write(&hi2c1, BMP280_ADDR, BMP280_REG_CTRL_MEAS,
                          1, &cfg, 1, 100) != HAL_OK)
        return 2;

    s_init_ok = 1;
    return 0;
}

/**
 * @brief  读温度 + 气压 + 海拔（forced 模式：每次读都要先触发一次转换）
 * @param  data 结果结构体
 * @retval 0=成功
 *         1=I2C 通信失败
 *         2=未初始化（忘了调 BMP280_Init）
 *         3=数据超出合理范围（兜底检查）
 *         4=空指针
 */
uint8_t BMP280_Read(BMP280_Data_t *data)
{
    uint8_t  buf[6];
    uint8_t  cfg;
    int32_t  adc_P, adc_T;
    int32_t  var1_t, var2_t;    /* 温度补偿中间量：32 位够用 */
    int64_t  var1, var2;        /* ★ 气压补偿中间量：必须 64 位！
                                    dig_P4<<35 ≈ 9.8e13，装不进 int32_t，
                                    用 32 位会被静默截断，气压直接错 8% */
    int64_t  p;
    float    t, press;

    if (data == NULL) return 4;
    if (!s_init_ok)   return 2;

    /* ① 触发一次转换（forced 模式）：写 ctrl_meas = 0x25 */
    cfg = BMP280_CTRL_MEAS_CFG;
    if (HAL_I2C_Mem_Write(&hi2c1, BMP280_ADDR, BMP280_REG_CTRL_MEAS,
                          1, &cfg, 1, 100) != HAL_OK)
        return 1;

    /* ② 等转换完成。
         严格做法是轮询 0xF3 的 bit3(measuring)，变 0 才算完；
         这里用固定延时（手册给的最大 6.5ms，取 7ms）足够且简单。 */
    delay_ms(BMP280_MEAS_DELAY_MS);

    /* ③ 从 0xF7 连续读 6 字节：
         buf[0..2] = 气压(20位)   buf[3..5] = 温度(20位)
         注意寄存器顺序是【气压在前、温度在后】，和读 ID 的顺序无关 */
    if (HAL_I2C_Mem_Read(&hi2c1, BMP280_ADDR, BMP280_REG_PRESS_MSB,
                         1, buf, 6, 100) != HAL_OK)
        return 1;

    /* ④ 20 位拼接：先把 3 字节拼成 24 位，再右移 4 位
         ⚠️ 忘了 >>4，数值会大 16 倍（气压变成 160 万 Pa 这种离谱值） */
    adc_P = (int32_t)((((uint32_t)buf[0] << 16) |
                       ((uint32_t)buf[1] << 8)  |
                        (uint32_t)buf[2]) >> 4);
    adc_T = (int32_t)((((uint32_t)buf[3] << 16) |
                       ((uint32_t)buf[4] << 8)  |
                        (uint32_t)buf[5]) >> 4);

    /* ⑤ 温度补偿（32 位够用） */
    var1_t = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    var2_t = (((((adc_T >> 4) - ((int32_t)dig_T1)) *
                ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) *
              ((int32_t)dig_T3)) >> 14;
    t_fine = var1_t + var2_t;

    t = (float)((t_fine * 5 + 128) >> 8) / 100.0f;   /* 单位 0.01℃ → ℃ */

    /* ⑥ 气压补偿 ⚠️ 全部用 int64，用 int32 会静默溢出 */
    var1 = ((int64_t)t_fine) - 128000;
    var2  = var1 * var1 * (int64_t)dig_P6;
    var2  = var2 + ((var1 * (int64_t)dig_P5) << 17);
    var2  = var2 + (((int64_t)dig_P4) << 35);
    var1  = ((var1 * var1 * (int64_t)dig_P3) >> 8) +
            ((var1 * (int64_t)dig_P2) << 12);
    var1  = (((((int64_t)1) << 47) + var1)) * ((int64_t)dig_P1) >> 33;

    if (var1 == 0) return 1;                         /* 防止除零 */

    p = 1048576 - adc_P;                             /* 2^20 */
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);

    press = (float)p / 256.0f;                       /* Q24.8 → Pa */

    /* ⑦ 范围检查：BMP280 没有 CRC，靠物理量范围兜底 */
    if (t < -40.0f || t > 85.0f)               return 3;
    if (press < 30000.0f || press > 110000.0f) return 3;   /* 300~1100 hPa */

    data->temperature = t;
    data->pressure    = press;
    data->altitude    = BMP280_CalcAltitude(press);

    return 0;
}

/**
 * @brief  气压 → 海拔（国际标准大气公式）
 * @param  pressure 实测气压，单位 Pa
 * @retval 海拔，单位 m
 * @note   需要 <math.h> 且 Keil 勾了 Use MicroLIB（powf 才能用）。
 *         ⚠️ 天气变化会让这个"海拔"漂几十米，别当绝对高度用，
 *            报告里写"相对高度变化"更严谨。
 */
float BMP280_CalcAltitude(float pressure)
{
    return 44330.0f * (1.0f - powf(pressure / 101325.0f, 0.1903f));
}
