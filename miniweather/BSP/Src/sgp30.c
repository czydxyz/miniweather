/* SGP30 气体传感器驱动（I2C 地址 0x58）
 * [约束] 不支持时钟拉伸，测量后必须 delay_ms(12)（手册给的是 max 12ms，勿用 typ 10ms）。
 * [约束] 必须每 1 秒测量一次，否则内部基线算法退化。
 * [约束] 输出为 CO2eq（CO2 当量），不是 CO2 浓度。
 * [约束] baseline 存取顺序相反：Get(CO2, TVOC)，Set(TVOC, CO2)；存 AT24C02，7 天有效。
 */
#include "sgp30.h"
#include "delay.h"
#include <math.h>

/* SGP30 传感器句柄使用 CubeMX 生成的 hi2c1 */
extern I2C_HandleTypeDef hi2c1;


/**
 * @brief  CRC-8（和 SHTC3 完全相同的参数：poly 0x31 / init 0xFF / 不反射）
 * @note   手册 Table 13 自检：CRC(0xBEEF) = 0x92
 */
static uint8_t SGP30_CRC8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0xFF;
    uint8_t i, j;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31)
                               : (uint8_t)(crc << 1);
    }
    return crc;
}

/**
 * @brief  发送 2 字节命令
 * @retval 0=成功  1=I2C 失败
 */
static uint8_t SGP30_SendCmd(uint16_t cmd)
{
    uint8_t buf[2];

    buf[0] = (uint8_t)(cmd >> 8);        /* 高字节先发 */
    buf[1] = (uint8_t)(cmd & 0xFF);

    if (HAL_I2C_Master_Transmit(&hi2c1, SGP30_ADDR, buf, 2, 100) != HAL_OK)
        return 1;
    return 0;
}

/**
 * @brief  读带 CRC 的响应（通用：n 个 word，每个 word = 2 数据 + 1 CRC）
 * @param  buf    缓冲区，长度必须是 words*3
 * @param  words  word 个数
 * @param  out    解析后的 word 数组，长度 = words
 * @retval 0=成功  1=读取失败  2=CRC 错误
 */
static uint8_t SGP30_ReadWords(uint8_t *buf, uint8_t words, uint16_t *out)
{
    uint8_t i;

    if (HAL_I2C_Master_Receive(&hi2c1, SGP30_ADDR, buf, words * 3, 100) != HAL_OK)
        return 1;

    for (i = 0; i < words; i++)
    {
        if (SGP30_CRC8(&buf[i * 3], 2) != buf[i * 3 + 2])
            return 2;                                  /* CRC 不过，整包丢弃 */

        out[i] = (uint16_t)((buf[i * 3] << 8) | buf[i * 3 + 1]);
    }
    return 0;
}

/**
 * @brief  初始化：发 Init_air_quality
 * @retval 0=成功  1=I2C 失败
 * @note   之后要等 15 秒预热，这期间读数固定 400ppm/0ppb（正常现象）。
 *         预热期间不要判定"传感器坏了"。
 */
uint8_t SGP30_Init(void)
{
    if (SGP30_SendCmd(SGP30_CMD_INIT_AIR_QUALITY) != 0)
        return 1;

    delay_ms(SGP30_DELAY_INIT_MS);       /* 手册 max 10ms */
    return 0;
}

/**
 * @brief  读 CO₂ 当量 + TVOC
 * @param  data 结果结构体
 * @retval 0=成功  1=通信失败  2=CRC 错误  3=超出量程  4=空指针
 * @note   单次耗时约 12ms。调用周期【必须约等于 1 秒】，
 *         传感器内部的基线算法靠这个节奏工作。
 */
uint8_t SGP30_Read(SGP30_Data_t *data)
{
    uint8_t  buf[6];
    uint16_t w[2];
    uint8_t  ret;

    if (data == NULL) return 4;

    /* ① 发测量命令 */
    if (SGP30_SendCmd(SGP30_CMD_MEASURE_AIR_QUALITY) != 0)
        return 1;

    /* ② 必须等满 12ms（按 max，不是 typ 的 10ms）
         SGP30 不支持时钟拉伸，等不够会直接被 XCK 中止 → 读回来全 0xFF */
    delay_ms(SGP30_DELAY_MEASURE_MS);

    /* ③ 读 6 字节并校验 CRC */
    ret = SGP30_ReadWords(buf, 2, w);
    if (ret != 0) return ret;            /* 1=通信失败 2=CRC错 */

    /* ④ 顺序：w[0]=CO2eq，w[1]=TVOC */
    if (w[0] > 60000 || w[1] > 60000) return 3;    /* 超量程，多半是读早了 */

    data->co2  = w[0];
    data->tvoc = w[1];

    return 0;
}

/**
 * @brief  读 48 位序列号（每片唯一，写报告/追溯用）
 * @param  id  6 字节数组（从 9 字节响应里抽出有效字节）
 * @retval 0=成功  1=通信失败  2=CRC 错误
 */
uint8_t SGP30_GetSerialID(uint8_t id[6])
{
    uint8_t  buf[9];
    uint16_t w[3];
    uint8_t  ret;

    if (SGP30_SendCmd(SGP30_CMD_GET_SERIAL_ID) != 0) return 1;

    delay_ms(SGP30_DELAY_ID_MS);         /* 手册 tIDLE = 0.5ms，10ms 保险 */

    ret = SGP30_ReadWords(buf, 3, w);
    if (ret != 0) return ret;

    /* 3 个 word = 6 字节有效数据，按大端拆开 */
    id[0] = (uint8_t)(w[0] >> 8);  id[1] = (uint8_t)(w[0] & 0xFF);
    id[2] = (uint8_t)(w[1] >> 8);  id[3] = (uint8_t)(w[1] & 0xFF);
    id[4] = (uint8_t)(w[2] >> 8);  id[5] = (uint8_t)(w[2] & 0xFF);

    return 0;
}

/**
 * @brief  传感器自检（Measure_test）
 * @retval 0=通过  1=失败（值不对）  2=通信/CRC 失败
 * @note   这个命令要 220ms，只在启动时跑一次，别放循环里
 */
uint8_t SGP30_MeasureTest(void)
{
    uint8_t  buf[3];
    uint16_t w[1];
    uint8_t  ret;

    if (SGP30_SendCmd(SGP30_CMD_MEASURE_TEST) != 0) return 2;

    delay_ms(SGP30_DELAY_TEST_MS);       /* 最慢的一个命令，220ms */

    ret = SGP30_ReadWords(buf, 1, w);
    if (ret != 0) return 2;

    return (w[0] == SGP30_TEST_PASS_VALUE) ? 0 : 1;
}

/**
 * @brief  读特性集版本（前 4 bit 是产品类型，SGP30 = 0x0）
 * @retval 0=成功  1=失败
 */
uint8_t SGP30_GetFeatureSet(uint16_t *version)
{
    uint8_t  buf[3];
    uint16_t w[1];
    uint8_t  ret;

    if (SGP30_SendCmd(SGP30_CMD_GET_FEATURE_SET) != 0) return 1;

    delay_ms(2);

    ret = SGP30_ReadWords(buf, 1, w);
    if (ret != 0) return 1;

    *version = w[0];
    return 0;
}

/**
 * @brief  读基线（用于掉电保存）
 * @param  co2_base   返回 CO2eq 基线
 * @param  tvoc_base  返回 TVOC 基线
 * @retval 0=成功  1=通信失败  2=CRC 错误
 * @note   返回顺序是【CO2eq 在前，TVOC 在后】
 *         需要连续运行 12 小时（且没有恢复过基线）才拿得到有意义的值
 */
uint8_t SGP30_GetBaseline(uint16_t *co2_base, uint16_t *tvoc_base)
{
    uint8_t  buf[6];
    uint16_t w[2];
    uint8_t  ret;

    if (SGP30_SendCmd(SGP30_CMD_GET_BASELINE) != 0) return 1;

    delay_ms(SGP30_DELAY_BASELINE_MS);

    ret = SGP30_ReadWords(buf, 2, w);
    if (ret != 0) return ret;

    *co2_base  = w[0];                  /* ★ CO2eq 在前 */
    *tvoc_base = w[1];
    return 0;
}

/**
 * @brief  写基线（上电恢复用）
 * @param  tvoc_base  TVOC 基线
 * @param  co2_base   CO2eq 基线
 * @retval 0=成功  1=I2C 失败
 * @note   ⚠️⚠️ 参数顺序和 GetBaseline 【相反】：这里是 TVOC 在前！
 *         手册 6.3 节分两句写的，极容易看漏。写反了不会报错，
 *         只是补偿值全乱，读数会越来越飘。
 *         基线有效期 7 天，超期要丢弃重新学习。
 */
uint8_t SGP30_SetBaseline(uint16_t tvoc_base, uint16_t co2_base)
{
    uint8_t buf[8];

    /* 帧格式 = 命令(2) + word0(2)+CRC(1) + word1(2)+CRC(1) = 8 字节
       ⚠️ 命令字本身也要占前 2 字节，别漏了 */
    buf[0] = (uint8_t)(SGP30_CMD_SET_BASELINE >> 8);
    buf[1] = (uint8_t)(SGP30_CMD_SET_BASELINE & 0xFF);

    buf[2] = (uint8_t)(tvoc_base >> 8);     /* ★ TVOC 在前 */
    buf[3] = (uint8_t)(tvoc_base & 0xFF);
    buf[4] = SGP30_CRC8(&buf[2], 2);

    buf[5] = (uint8_t)(co2_base >> 8);
    buf[6] = (uint8_t)(co2_base & 0xFF);
    buf[7] = SGP30_CRC8(&buf[5], 2);

    if (HAL_I2C_Master_Transmit(&hi2c1, SGP30_ADDR, buf, 8, 100) != HAL_OK)
        return 1;

    delay_ms(SGP30_DELAY_BASELINE_MS);
    return 0;
}

/**
 * @brief  用相对湿度和温度算出绝对湿度（只算不写）
 * @param  humidity_rh 相对湿度 %RH（来自 SHTC3）
 * @param  temp_c      温度 ℃（来自 SHTC3）
 * @retval 8.8 定点格式的绝对湿度（= 真实 g/m³ × 256）
 * @note   公式（官方集成指南 p.5）：
 *           AH = 216.7 × (RH/100 × 6.112 × exp(17.62t/(243.12+t))) / (273.15+t)
 *         实测锚点：25.63℃ / 58%RH → 13.801 g/m³ → 0x0DCD
 *         需要 <math.h> + Keil 勾 Use MicroLIB（expf）
 */
uint16_t SGP30_CalcAbsoluteHumidity(float humidity_rh, float temp_c)
{
    float ah;        /* 绝对湿度，g/m³ */

    if (humidity_rh < 0.0f)   humidity_rh = 0.0f;
    if (humidity_rh > 100.0f) humidity_rh = 100.0f;

    ah = 216.7f * ((humidity_rh / 100.0f) * 6.112f *
                   expf((17.62f * temp_c) / (243.12f + temp_c)) /
                   (273.15f + temp_c));

    /* 转成 8.8 定点：整数部分放高 8 位，小数部分放低 8 位 */
    return (uint16_t)(ah * 256.0f);
}

/**
 * @brief  把 SHTC3 的温湿度喂给 SGP30 做湿度补偿
 * @param  humidity_rh 相对湿度 %RH
 * @param  temp_c      温度 ℃
 * @retval 0=成功  1=I2C 失败
 * @note   湿度变化很慢，【不用每次读 SGP30 都写】，
 *         一分钟写一次就够（每次写要多占 1~10ms 总线时间）。
 *         不写的话传感器内部默认按 11.57 g/m³ 补偿，不是不补偿。
 */
uint8_t SGP30_SetAbsoluteHumidity(float humidity_rh, float temp_c)
{
    uint8_t  buf[5];
    uint16_t ah_scaled;

    ah_scaled = SGP30_CalcAbsoluteHumidity(humidity_rh, temp_c);

    /* 帧格式 = 命令(2) + 参数(2) + CRC(1) = 5 字节 */
    buf[0] = (uint8_t)(SGP30_CMD_SET_HUMIDITY >> 8);
    buf[1] = (uint8_t)(SGP30_CMD_SET_HUMIDITY & 0xFF);

    buf[2] = (uint8_t)(ah_scaled >> 8);
    buf[3] = (uint8_t)(ah_scaled & 0xFF);
    buf[4] = SGP30_CRC8(&buf[2], 2);

    if (HAL_I2C_Master_Transmit(&hi2c1, SGP30_ADDR, buf, 5, 100) != HAL_OK)
        return 1;

    delay_ms(10);
    return 0;
}
