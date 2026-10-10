/* 应用层：上电自检 / 屏幕界面 / 按键交互 / 自动联动决策
 * [约束] 手动操作后 APP_MANUAL_HOLD_S(30s) 内自动逻辑不介入。
 * [约束] 行槽 y 坐标一律用枚举，禁止硬编码常量。
 */
#include "app.h"

#include "lcd.h"
#include "delay.h"
#include "i2c.h"          /* hi2c1：自检开头做 I2C 总线扫描要用 */
#include <stdio.h>
#include <string.h>

#include "shtc3.h"
#include "bmp280.h"
#include "sgp30.h"
#include "at24c02.h"
#include "w25q128.h"
#include "flash_if.h"
#include "servo.h"
#include "relay.h"
#include "beep.h"
#include "ai_client.h"      /* AI 顾问：AI_TakeAdvice() 取大模型建议文本 */
#include "key.h"
#include "led.h"
#include "comm.h"
#include "esp8266.h"

/* 内部状态 */
static AppState_t s_state;
static uint32_t   s_uptime_s = 0;

/* 手动模式持续时长（秒）：按过按键后这段时间内自动逻辑不插手，
   否则你刚手动开窗，下一秒程序就给你关上了 */
#define APP_MANUAL_HOLD_S       30u

AppState_t *APP_GetState(void)      { return &s_state; }
void        APP_SetUptime(uint32_t sec) { s_uptime_s = sec; }
uint32_t    APP_GetUptime(void)     { return s_uptime_s; }


/* 结果行缓冲，打印的同时抄一份到 LCD */
static uint8_t s_pass;
static uint8_t s_total;

static void TestLine(uint8_t idx, const char *name, uint8_t ok, const char *detail)
{
    printf("[%u] %-8s %-22s %s\r\n",
           (unsigned int)idx, name, (detail ? detail : ""), ok ? "PASS" : "FAIL");

    s_total++;
    if (ok) s_pass++;

    /* LCD 上一行 8×16 字体，最多 30 个字符（240/8 = 30） */
    {
        char line[32];
        uint16_t y = 120 + (uint16_t)(s_total - 1) * 18;

        snprintf(line, sizeof(line), "%-14s%6s %s",
                 name, ok ? "OK" : "ERR", (detail ? detail : ""));
        line[29] = '\0';                       /* 截断到屏幕宽度 */
        LCD_ShowString(4, y, line,
                       ok ? GREEN : RED, BLACK);
    }
}

uint8_t APP_SelfTest(uint8_t *total)
{
    uint8_t  idx = 0;
    uint8_t  buf8[8];
    uint32_t id32;
    int      rc;
    char     msg[64];

    s_pass = 0;
    s_total = 0;

    LCD_Clear(BLACK);
    LCD_ShowString(4, 8,  "MiniWeather Self-Test", CYAN, BLACK);
    LCD_ShowString(4, 28, "USART1 115200 for log", GRAY, BLACK);
    LCD_DrawLine(4, 48, 236, 48, GRAY);
    LCD_ShowString(4, 60, "start...", YELLOW, BLACK);

    printf("\r\n");
    printf("========================================\r\n");
    printf(" MiniWeather Hardware Self-Test\r\n");
    printf("========================================\r\n");

    {
        uint8_t a, found = 0;
        printf("[SCAN ] I2C1 bus: ");
        for (a = 0x08; a <= 0x77; a++) {
            if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1), 1, 5) == HAL_OK) {
                printf("0x%02X ", (unsigned int)a);
                found++;
            }
        }
        if (!found) printf("(none! bus dead)");
        printf("\r\n");
    }

    /* ---------- 1. SHTC3 温湿度 ---------- */
    {
        SHTC3_Data_t th;
        idx++;
        if (SHTC3_Read(&th) == 0) {
            snprintf(msg, sizeof(msg), "%.1fC %.1f%%", th.temperature, th.humidity);
            TestLine(idx, "SHTC3", 1, msg);
        } else {
            TestLine(idx, "SHTC3", 0, "no ack / CRC err");
        }
    }

    /* ---------- 2. BMP280 气压 ---------- */
    {
        BMP280_Data_t bp;
        uint8_t  bid = 0;
        idx++;
        BMP280_ReadID(&bid);
        if (BMP280_Read(&bp) == 0) {
            snprintf(msg, sizeof(msg), "%s %.0fPa",
                     BMP280_IsBME280() ? "BME280" : "BMP280", bp.pressure);
            TestLine(idx, "BMP280", 1, msg);
        } else {
            snprintf(msg, sizeof(msg), "id=0x%02X fail", bid);
            TestLine(idx, "BMP280", 0, msg);
        }
    }

    /* ---------- 3. SGP30 空气质量 ---------- */
    {
        SGP30_Data_t air;
        idx++;
        /* 注意：上电 15 秒内固定输出 400/0，这是预热期不是故障 */
        if (SGP30_Read(&air) == 0) {
            snprintf(msg, sizeof(msg), "CO2eq %u TVOC %u",
                     (unsigned int)air.co2, (unsigned int)air.tvoc);
            TestLine(idx, "SGP30", 1, msg);
        } else {
            TestLine(idx, "SGP30", 0, "no ack (warmup?)");
        }
    }

    /* ---------- 4. AT24C02 EEPROM ---------- */
    {
        uint8_t i, ok;
        idx++;
        for (i = 0; i < 8; i++) buf8[i] = (uint8_t)(0xA0 + i);
        AT24C02_WriteBytes(TEST_EEPROM_ADDR, buf8, 8);
        delay_ms(10);                                /* 写周期，器件期间不应答 */
        memset(buf8, 0, sizeof(buf8));
        AT24C02_ReadBytes(TEST_EEPROM_ADDR, buf8, 8);
        ok = 1;
        for (i = 0; i < 8; i++) if (buf8[i] != (uint8_t)(0xA0 + i)) ok = 0;
        snprintf(msg, sizeof(msg), "0x%02X: %02X %02X..",
                 (unsigned int)TEST_EEPROM_ADDR,
                 (unsigned int)buf8[0], (unsigned int)buf8[1]);
        TestLine(idx, "AT24C02", ok, msg);
    }

    /* ---------- 5. W25Q128 SPI Flash ---------- */
    {
        uint8_t  wbuf[16], rbuf[16];
        uint8_t  ok;
        idx++;
        id32 = W25Q128_ReadID();
        if (!W25Q128_IDIsValid(id32)) {
            snprintf(msg, sizeof(msg), "%s id=0x%06lX",
                     W25Q128_IDName(id32), (unsigned long)id32);
            TestLine(idx, "W25Q128", 0, msg);
        } else {
            memset(wbuf, 0, sizeof(wbuf));
            memset(rbuf, 0, sizeof(rbuf));
            for (uint8_t i = 0; i < 16; i++) wbuf[i] = (uint8_t)(0x50 + i);

            /* ★ 顺序不能反：必须先擦成 0xFF 才能写，否则写不进去 */
            W25Q128_EraseSector(TEST_W25Q_ADDR);
            W25Q128_Write(TEST_W25Q_ADDR, wbuf, 16);
            W25Q128_Read(TEST_W25Q_ADDR, rbuf, 16);

            ok = (memcmp(wbuf, rbuf, 16) == 0);
            snprintf(msg, sizeof(msg), "%s %02X %02X..",
                     W25Q128_IDName(id32),
                     (unsigned int)rbuf[0], (unsigned int)rbuf[1]);
            TestLine(idx, "W25Q128", ok, msg);
        }
    }

    /* ---------- 6. 内部 Flash（擦 + 写 + 读） ---------- */
    {
        uint8_t  wbuf[8], rbuf[8];
        uint8_t  ok;
        uint8_t  i;
        idx++;
        memset(wbuf, 0, sizeof(wbuf));
        memset(rbuf, 0, sizeof(rbuf));
        for (i = 0; i < 8; i++) wbuf[i] = (uint8_t)(0x11 + i);

        rc = FLASH_IF_EraseSectorByAddr(TEST_FLASH_ADDR);
        if (rc == 0) rc = FLASH_IF_Write(TEST_FLASH_ADDR, wbuf, 8);
        FLASH_IF_Read(TEST_FLASH_ADDR, rbuf, 8);

        ok = (rc == 0) && (memcmp(wbuf, rbuf, 8) == 0);
        snprintf(msg, sizeof(msg), "S11 %02X %02X..",
                 (unsigned int)rbuf[0], (unsigned int)rbuf[1]);
        TestLine(idx, "INT-FLS", ok, msg);
    }

    /* ---------- 7. 自定义协议（直接喂帧，看有没有应答） ---------- */
    {
        uint32_t tx_before;
        uint8_t  ok;
        idx++;
        tx_before = Comm_GetCtx()->tx_frames;

        Comm_OnFrame(0x0001, NULL, 0);        /* 查版本 */
        Comm_OnFrame(0x0007, NULL, 0);        /* 读传感器 */
        {
            uint8_t pl[2] = { DEV_BEEP, 0 };  /* 蜂鸣器关（不发声音，只验证通路） */
            Comm_OnFrame(0x0008, pl, 2);
        }

        ok = (Comm_GetCtx()->tx_frames - tx_before) == 3u;
        snprintf(msg, sizeof(msg), "tx +%lu",
                 (unsigned long)(Comm_GetCtx()->tx_frames - tx_before));
        TestLine(idx, "PROTO", ok, msg);
    }

    /* ---------- 8. 执行器：继电器 + 蜂鸣器 ---------- */
    {
        idx++;
        RELAY_Set(1);  delay_ms(300);
        RELAY_Set(0);  delay_ms(100);
        BEEP_Beep(80);                        /* 非阻塞，响 80ms */
        delay_ms(200);
        TestLine(idx, "RELAY", 1, "click + beep");
    }

    /* ---------- 9. 执行器：舵机 ---------- */
    {
        idx++;
#if (SERVO_TYPE == SERVO_TYPE_360)
        /* 360° 舵机没有位置反馈，只能"转一段时间"：正转 0.6s 再反转 0.6s */
        Servo_SetSpeed(60);   delay_ms(600);
        Servo_SetSpeed(-60);  delay_ms(600);
        Servo_SetSpeed(0);
        TestLine(idx, "SERVO", 1, "360 cw+ccw 0.6s");
#else
        Servo_SetAngle(90);   delay_ms(600);
        Servo_SetAngle(0);    delay_ms(600);
        TestLine(idx, "SERVO", 1, "180 90deg->0deg");
#endif
    }

    /* ---------- 汇总 ---------- */
    printf("----------------------------------------\r\n");
    printf(" RESULT: %u/%u PASS\r\n", (unsigned int)s_pass, (unsigned int)s_total);
    printf("========================================\r\n\r\n");

    LCD_ShowString(4, 280, "RESULT:", WHITE, BLACK);
    LCD_ShowInt(80, 280, s_pass, 2, (s_pass == s_total) ? GREEN : RED, BLACK);
    LCD_ShowString(100, 280, "/", WHITE, BLACK);
    LCD_ShowInt(110, 280, s_total, 2, WHITE, BLACK);
    LCD_ShowString(150, 280, (s_pass == s_total) ? "ALL OK" : "CHECK",
                   (s_pass == s_total) ? GREEN : RED, BLACK);
    delay_ms(1500);

    if (total) *total = s_total;
    return s_pass;
}

#define BASELINE_EEPROM_ADDR    0x10u
#define BASELINE_MAGIC          0x5A5Au
/* 12 小时后基线才可信 */
#define BASELINE_VALID_S        (12u * 3600u)
/* 之后每小时存一次 */
#define BASELINE_SAVE_INTERVAL  (1u * 3600u)

static uint8_t s_baseline_loaded;

void APP_BaselineTick(uint32_t sec)
{
    uint16_t co2_base, tvoc_base;
    uint8_t  buf[8];

    /* ---- 上电第一次：把 EEPROM 里的旧基线写回传感器 ---- */
    if (!s_baseline_loaded) {
        s_baseline_loaded = 1;

        AT24C02_ReadBytes(BASELINE_EEPROM_ADDR, buf, 8);

        if ((((uint16_t)buf[0] << 8) | buf[1]) == BASELINE_MAGIC) {
            uint16_t sum = 0, want;
            uint8_t  i;
            for (i = 0; i < 6; i++) sum = (uint16_t)(sum + buf[i]);
            want = (uint16_t)(((uint16_t)buf[6] << 8) | buf[7]);

            if (sum == want) {
                co2_base  = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);
                tvoc_base = (uint16_t)(((uint16_t)buf[4] << 8) | buf[5]);
                /* ★ 参数是 (TVOC, CO2)，顺序和读出来时相反 */
                if (SGP30_SetBaseline(tvoc_base, co2_base) == 0) {
                    printf("[SGP30 ] baseline restored CO2=%u TVOC=%u\r\n",
                           (unsigned int)co2_base, (unsigned int)tvoc_base);
                }
            }
        } else {
            printf("[SGP30 ] no saved baseline, need 12h to learn\r\n");
        }
        return;
    }

    /* ---- 满 12 小时后：每小时存一次 ---- */
    if (sec < BASELINE_VALID_S) return;
    if ((sec % BASELINE_SAVE_INTERVAL) != 0u) return;

    if (SGP30_GetBaseline(&co2_base, &tvoc_base) != 0) return;
    /* 全 0 是无效基线（传感器还没算出来） */
    if (co2_base == 0u && tvoc_base == 0u) return;

    buf[0] = (uint8_t)(BASELINE_MAGIC >> 8);
    buf[1] = (uint8_t)(BASELINE_MAGIC & 0xFF);
    buf[2] = (uint8_t)(co2_base  >> 8);
    buf[3] = (uint8_t)(co2_base  & 0xFF);
    buf[4] = (uint8_t)(tvoc_base >> 8);
    buf[5] = (uint8_t)(tvoc_base & 0xFF);
    {
        uint16_t sum = 0;
        uint8_t  i;
        for (i = 0; i < 6; i++) sum = (uint16_t)(sum + buf[i]);
        buf[6] = (uint8_t)(sum >> 8);
        buf[7] = (uint8_t)(sum & 0xFF);
    }
    AT24C02_WriteBytes(BASELINE_EEPROM_ADDR, buf, 8);
    printf("[SGP30 ] baseline saved CO2=%u TVOC=%u\r\n",
           (unsigned int)co2_base, (unsigned int)tvoc_base);
}


/* ---------------- 布局常量：只改这里就能整体调整版面 ---------------- */
#define UI_FONT_W        8u                      /* 字符宽（像素） */
#define UI_FONT_H        16u                     /* 字符高（像素） */
#define UI_LINE_H        19u                     /* ★ 行距，必须 > UI_FONT_H */
#define UI_TOP           26u                     /* 第 0 行的 y */
#define UI_ROW(n)        ((uint16_t)(UI_TOP + (n) * UI_LINE_H))
#define UI_X(col)        ((uint16_t)((col) * UI_FONT_W))   /* 字符列 → 像素 x */

/* 屏幕最多能放多少行（向下取整，保证最后一个字符完整落在屏内） */
#define UI_ROW_MAX       ((LCD_H - UI_TOP) / UI_LINE_H)    /* (320-26)/19 = 15 */

/* 列槽（单位：字符列，0~29） */
#define UI_C_LABEL       1u                      /* 左半：标签 */
#define UI_C_VALUE       8u                      /* 左半：数值 */
#define UI_C_UNIT        15u                     /* 左半：单位（3 列宽） */
#define UI_C_R_LABEL     18u                     /* 右半：标签 */
#define UI_C_R_VALUE     24u                     /* 右半：数值（24+6=30 列，正好到屏边） */

/* 固定宽度（字符数）—— 不足补空格，超了截断 */
#define UI_W_LABEL       7u
#define UI_W_VALUE       6u
#define UI_W_UNIT        3u
#define UI_W_STATE       6u
#define UI_W_TIME        8u
#define UI_W_LINE        28u                     /* 整行文本（按键提示等） */

/* 行槽枚举 —— 加行就在这里加，然后看编译期检查过不过 */
typedef enum {
    ROW_TEMP = 0,
    ROW_HUMI,
    ROW_PRESS,
    ROW_CO2,
    ROW_TVOC,
    ROW_SEP1,           /* 分隔线（只占槽位，不画字符） */
    ROW_STATE1,         /* Window | Fan   */
    ROW_STATE2,         /* Alarm  | Manual */
    ROW_SEP2,           /* 分隔线 */
    ROW_WIFI,
    ROW_UPTIME,
    ROW_AI,             /* AI 顾问建议（超屏宽时横向滚动） */
    ROW_KEY1,
    ROW_KEY2,
    UI_ROW_COUNT        /* 不是行，是总数 = 15，正好用满 UI_ROW_MAX，再加行会编译报错 */
} UI_Row_t;

/* ★ 编译期检查：行数超屏就直接编译失败（typedef 数组技巧，不占任何内存） */
typedef char ui_row_overflow_check[(UI_ROW_COUNT <= UI_ROW_MAX) ? 1 : -1];

/*
 * 固定宽度文本输出 —— 本界面【唯一】的文本出口
 * width = 0 表示不补空格（只用于静态标签，它们长度恒定、不会残留）
 * 其余一律填固定宽度：这是"不重叠"的核心机制。
  */
static void UI_Text(uint16_t x, uint16_t y, const char *str, uint8_t width,
                    uint16_t fc, uint16_t bc)
{
    char    buf[32];
    uint8_t i = 0u;

    if (str == NULL) str = "";

    if (width == 0u) {                       /* 不补齐模式 */
        LCD_ShowString(x, y, str, fc, bc);
        return;
    }

    if (width > (uint8_t)(sizeof(buf) - 1u)) width = (uint8_t)(sizeof(buf) - 1u);

    while (*str != '\0' && i < width) {      /* 拷贝，超出部分直接截断 */
        buf[i++] = *str++;
    }
    while (i < width) {                      /* ★ 不足补空格：把旧字符用背景色盖掉 */
        buf[i++] = ' ';
    }
    buf[i] = '\0';

    LCD_ShowString(x, y, buf, fc, bc);
}

void APP_Display(void)
{
    SensorSnapshot_t s;
    char             line[24];
    static uint8_t   first_frame = 1u;       /* 首帧标志：只在上电后清一次全屏 */

    /* ★ 必须走 Comm_GetSensorSnapshot()，不能直接 `Comm_GetCtx()->sensor`：
       那条路径绕过临界区。Display 优先级(4) 低于 Sensor(5)，
       拷 20 字节的中途被抢占就会拿到"温度是新的、气压还是上轮"的混合体。 */
    Comm_GetSensorSnapshot(&s);

    /* ① 首帧全屏清 —— 清掉上电残留 / 自检界面留下的像素 */
    if (first_frame) {
        LCD_Fill(0, 0, LCD_W - 1u, LCD_H - 1u, BLACK);
        first_frame = 0u;
    }

    /* ② 标题栏（静态，每次重画也无所谓，就 24 行像素） */
    LCD_Fill(0, 0, LCD_W - 1u, 24u, BLUE);
    UI_Text(UI_X(1), 4u, "MiniWeather", 0, WHITE, BLUE);
    UI_Text(UI_X(22), 4u, FIRMWARE_VERSION, 0, WHITE, BLUE);

    /* ③ 传感器区：标签固定宽 + 数值固定宽 + 单位固定宽 */
    UI_Text(UI_X(UI_C_LABEL), UI_ROW(ROW_TEMP),  "Temp ", UI_W_LABEL, CYAN,  BLACK);
    /* intlen=4 + '.' + dec=1 = 6 字符，和下面 ShowInt(…,6) 宽度一致，
       这样整列的右端是对齐的，也保证位数变化时不会留残影 */
    LCD_ShowFloat(UI_X(UI_C_VALUE), UI_ROW(ROW_TEMP),
                  (float)s.temp_01 / 10.0f, 4, 1, WHITE, BLACK);
    UI_Text(UI_X(UI_C_UNIT), UI_ROW(ROW_TEMP),   "C",    UI_W_UNIT,  CYAN,  BLACK);

    UI_Text(UI_X(UI_C_LABEL), UI_ROW(ROW_HUMI),  "Humi ", UI_W_LABEL, CYAN,  BLACK);
    LCD_ShowFloat(UI_X(UI_C_VALUE), UI_ROW(ROW_HUMI),
                  (float)s.humi_01 / 10.0f, 4, 1, WHITE, BLACK);
    UI_Text(UI_X(UI_C_UNIT), UI_ROW(ROW_HUMI),   "%RH",  UI_W_UNIT,  CYAN,  BLACK);

    UI_Text(UI_X(UI_C_LABEL), UI_ROW(ROW_PRESS), "Press", UI_W_LABEL, CYAN,  BLACK);
    LCD_ShowInt  (UI_X(UI_C_VALUE), UI_ROW(ROW_PRESS),
                  (int32_t)s.press_pa, UI_W_VALUE, WHITE, BLACK);
    UI_Text(UI_X(UI_C_UNIT), UI_ROW(ROW_PRESS),  "Pa",   UI_W_UNIT,  CYAN,  BLACK);

    UI_Text(UI_X(UI_C_LABEL), UI_ROW(ROW_CO2),   "CO2eq", UI_W_LABEL, CYAN,  BLACK);
    LCD_ShowInt  (UI_X(UI_C_VALUE), UI_ROW(ROW_CO2), (int32_t)s.co2_ppm, UI_W_VALUE,
                  (s.co2_ppm > CTRL_CO2_ALARM_PPM) ? RED : GREEN, BLACK);
    UI_Text(UI_X(UI_C_UNIT), UI_ROW(ROW_CO2),    "ppm",  UI_W_UNIT,  CYAN,  BLACK);

    UI_Text(UI_X(UI_C_LABEL), UI_ROW(ROW_TVOC),  "TVOC ", UI_W_LABEL, CYAN,  BLACK);
    LCD_ShowInt  (UI_X(UI_C_VALUE), UI_ROW(ROW_TVOC),
                  (int32_t)s.tvoc_ppb, UI_W_VALUE, WHITE, BLACK);
    UI_Text(UI_X(UI_C_UNIT), UI_ROW(ROW_TVOC),   "ppb",  UI_W_UNIT,  CYAN,  BLACK);

    /* ④ 设备状态区：两列并排，省下 2 行 */
    LCD_DrawLine(UI_X(1), UI_ROW(ROW_SEP1) + 8u, UI_X(28), UI_ROW(ROW_SEP1) + 8u, GRAY);

    UI_Text(UI_X(UI_C_LABEL), UI_ROW(ROW_STATE1), "Window", UI_W_LABEL, CYAN, BLACK);
    UI_Text(UI_X(UI_C_VALUE), UI_ROW(ROW_STATE1),
            s_state.window_open ? "OPEN " : "CLOSE", UI_W_STATE,
            s_state.window_open ? GREEN : GRAY, BLACK);

    UI_Text(UI_X(UI_C_R_LABEL), UI_ROW(ROW_STATE1), "Fan", 0, CYAN, BLACK);
    UI_Text(UI_X(UI_C_R_VALUE), UI_ROW(ROW_STATE1),
            s_state.fan_on ? "ON " : "OFF", UI_W_STATE,
            s_state.fan_on ? GREEN : GRAY, BLACK);

    UI_Text(UI_X(UI_C_LABEL), UI_ROW(ROW_STATE2), "Alarm ", UI_W_LABEL, CYAN, BLACK);
    UI_Text(UI_X(UI_C_VALUE), UI_ROW(ROW_STATE2),
            s_state.alarming ? "YES" : "NO ", UI_W_STATE,
            s_state.alarming ? RED : GRAY, BLACK);

    UI_Text(UI_X(UI_C_R_LABEL), UI_ROW(ROW_STATE2), "Man", 0, CYAN, BLACK);
    UI_Text(UI_X(UI_C_R_VALUE), UI_ROW(ROW_STATE2),
            s_state.manual ? "YES" : "NO ", UI_W_STATE,
            s_state.manual ? YELLOW : GRAY, BLACK);

    /* ⑤ 网络 + 运行时间 */
    LCD_DrawLine(UI_X(1), UI_ROW(ROW_SEP2) + 8u, UI_X(28), UI_ROW(ROW_SEP2) + 8u, GRAY);

    {
        ESP8266_Status_t st = ESP8266_GetStatus();
        const char *ws = (st == ESP_TCP_CONNECTED)    ? "TCP " :
                         (st == ESP_WIFI_CONNECTED)   ? "WIFI" :
                         (st == ESP_TCP_DISCONNECTED) ? "TCP-" : "OFF ";
        UI_Text(UI_X(UI_C_LABEL), UI_ROW(ROW_WIFI), "WiFi  ", UI_W_LABEL, CYAN, BLACK);
        /* ★ 这里最需要固定宽度：4 字符的 "WIFI" 变 3 字符的 "OFF" 时，
              不补齐就会残留一个 'I' —— 这就是之前"重叠"的元凶之一。 */
        UI_Text(UI_X(UI_C_VALUE), UI_ROW(ROW_WIFI), ws, UI_W_STATE,
                (st >= ESP_WIFI_CONNECTED) ? GREEN : RED, BLACK);
    }

    UI_Text(UI_X(UI_C_LABEL), UI_ROW(ROW_UPTIME), "Up    ", UI_W_LABEL, CYAN, BLACK);
    snprintf(line, sizeof(line), "%02lu:%02lu:%02lu",
             (unsigned long)(s_uptime_s / 3600u),
             (unsigned long)((s_uptime_s / 60u) % 60u),
             (unsigned long)(s_uptime_s % 60u));
    UI_Text(UI_X(UI_C_VALUE), UI_ROW(ROW_UPTIME), line, UI_W_TIME, WHITE, BLACK);

    {
        /* static：建议内容是"跨帧记忆"的 —— 取到一条就一直显示到下一条来 */
        static char     s_advice[AI_ADVICE_MAX];
        static uint16_t s_scroll = 0u;      /* 当前滚动位置（字符） */
        static uint8_t  s_tick   = 0u;      /* 500ms 计数，凑 2 秒 */

        if (AI_TakeAdvice(s_advice, (uint16_t)sizeof(s_advice))) {
            printf("[AI    ] %s\r\n", s_advice);
            s_scroll = 0u;                  /* 新建议从头开始滚 */
        }

        if (s_advice[0] == '\0') {
            UI_Text(UI_X(1), UI_ROW(ROW_AI), "AI: waiting for advice...",
                    UI_W_LINE, LGRAY, BLACK);
        } else {
            char     win[UI_W_LINE + 1u];
            uint16_t len  = (uint16_t)strlen(s_advice);
            uint16_t span = (uint16_t)(len + 4u);   /* +4：末尾留空隙，滚起来不粘连 */
            uint16_t k;

            if (++s_tick >= 4u) { s_tick = 0u; s_scroll++; }
            if (s_scroll >= span) s_scroll = 0u;

            for (k = 0u; k < UI_W_LINE; k++) {
                uint16_t idx = (uint16_t)(s_scroll + k);
                if (idx >= span) idx = (uint16_t)(idx - span);
                win[k] = (idx < len) ? s_advice[idx] : ' ';
            }
            win[UI_W_LINE] = '\0';

            UI_Text(UI_X(1), UI_ROW(ROW_AI), win, UI_W_LINE, YELLOW, BLACK);
        }
    }

    /* ⑥ 按键提示（静态文本，长度恒定，不必补齐） */
    UI_Text(UI_X(UI_C_LABEL), UI_ROW(ROW_KEY1), "K0:open  K1:close", 0, LGRAY, BLACK);
    UI_Text(UI_X(UI_C_LABEL), UI_ROW(ROW_KEY2), "K2:beep  KUP:fan ", 0, LGRAY, BLACK);
}

/*
 * 三、按键交互
 *   按过任意键就进入"手动模式"30 秒，期间自动控制不再干预，
 *   否则你刚开窗程序立刻给你关上（这个坑几乎人人踩）
  */
void APP_KeyPoll(void)
{
    uint8_t k = KEY_Scan(0);

    if (k == KEY_NONE) return;

    s_state.manual = 1;
    s_state.manual_until_s = s_uptime_s + APP_MANUAL_HOLD_S;

    switch (k) {
    case KEY0_PRESS:
        if (!s_state.window_open) {
            Servo_Open();
            s_state.window_open = 1;
            printf("[KEY0] window OPEN\r\n");
        }
        break;

    case KEY1_PRESS:
        if (s_state.window_open) {
            Servo_Close();
            s_state.window_open = 0;
            printf("[KEY1] window CLOSE\r\n");
        }
        break;

    case KEY2_PRESS:
        BEEP_AlarmStart(2, 100, 100);
        printf("[KEY2] beep x2\r\n");
        break;

    case KEYUP_PRESS:
        s_state.fan_on = s_state.fan_on ? 0u : 1u;
        RELAY_Set(s_state.fan_on);
        printf("[KEYUP] fan %s\r\n", s_state.fan_on ? "ON" : "OFF");
        break;

    default:
        break;
    }
}

void APP_CtrlTick(void)
{
    SensorSnapshot_t s;
    uint8_t want_window, want_fan, want_alarm;
    static uint32_t last_alarm_s = 0xFFFFFFFFu;

    /* 手动模式到期自动退出 */
    if (s_state.manual && (s_uptime_s > s_state.manual_until_s)) {
        s_state.manual = 0;
        printf("[CTRL] back to auto\r\n");
    }
    if (s_state.manual) return;

    Comm_GetSensorSnapshot(&s);   /* ★ 同上，读侧也必须在临界区 */
    if (s.tick == 0) return;              /* 还没有拿到过传感器数据 */

    want_window = s_state.window_open;
    want_fan    = s_state.fan_on;
    want_alarm  = s_state.alarming;

    /* --- CO₂ 判断（带回差） --- */
    if (s.co2_ppm > CTRL_CO2_ALARM_PPM) {
        want_window = 1;
        want_fan    = 1;
        want_alarm  = 1;
    } else if (s.co2_ppm < CTRL_CO2_RECOVER_PPM) {
        want_window = 0;
        want_alarm  = 0;
    }

    /* --- 温度判断（只管风扇） --- */
    if (s.temp_01 > CTRL_TEMP_ALARM_01) {
        want_fan = 1;
    } else if ((s.temp_01 < CTRL_TEMP_RECOVER_01) &&
               (s.co2_ppm < CTRL_CO2_RECOVER_PPM)) {
        want_fan = 0;
    }

    /* --- 落地：只在状态【发生变化】时才动作，避免每 500ms 重复下发 --- */
    if (want_window != s_state.window_open) {
        if (want_window) Servo_Open(); else Servo_Close();
        s_state.window_open = want_window;
        printf("[CTRL] window %s (CO2=%u)\r\n",
               want_window ? "OPEN" : "CLOSE", (unsigned int)s.co2_ppm);
    }

    if (want_fan != s_state.fan_on) {
        RELAY_Set(want_fan);
        s_state.fan_on = want_fan;
        printf("[CTRL] fan %s (T=%.1f)\r\n",
               want_fan ? "ON" : "OFF", (float)s.temp_01 / 10.0f);
    }

    if (want_alarm && !s_state.alarming) {
        /* 两次报警之间至少间隔 CTRL_ALARM_INTERVAL_S 秒，否则会一直响 */
        if ((last_alarm_s == 0xFFFFFFFFu) ||
            (s_uptime_s - last_alarm_s >= CTRL_ALARM_INTERVAL_S)) {
            BEEP_AlarmStart(3, 200, 200);
            last_alarm_s = s_uptime_s;
            printf("[CTRL] ALARM CO2=%u\r\n", (unsigned int)s.co2_ppm);
        }
        s_state.alarming = 1;
    } else if (!want_alarm) {
        s_state.alarming = 0;
    }
}
