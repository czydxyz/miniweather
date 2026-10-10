#include "lcd_test.h"
#include "lcd.h"
#include "delay.h"
#include <stdio.h>          /* printf → 重定向到 USART1（见 usart.c 的 fputc）*/


#define COLOR_TITLE   0x0010
#define COLOR_BAR     0x4208
#define COLOR_GRID    0x8410

static void test_l1_backlight(void)
{
    LCD_BL_OFF();
    delay_ms(500);
    printf("[LCD] L1 背光 OFF  ← 屏幕此刻应该明显变暗（贴很近才能看见字）\r\n");

    LCD_BL_ON();
    delay_ms(500);
    printf("[LCD] L1 背光 ON   ← 屏幕应该变亮（此时画面还是全黑）\r\n");
}

static void test_l2_colors(void)
{
    static const uint16_t c[5] = {RED, GREEN, BLUE, WHITE, BLACK};
    static const char    *n[5] = {"RED  ", "GREEN", "BLUE ", "WHITE", "BLACK"};
    uint8_t i;

    for (i = 0; i < 5; i++)
    {
        printf("[LCD] L2 全屏填充 -> %s\r\n", n[i]);
        LCD_Clear(c[i]);
        delay_ms(350);
    }
}

static void test_l3_direction(void)
{
    /* 三色竖条 */
    LCD_Fill(0,   0, 79,  319, RED);
    LCD_Fill(80,  0, 159, 319, GREEN);
    LCD_Fill(160, 0, 239, 319, BLUE);

    /* 四角 L 形白标，用来判断"哪边是上、哪边是左" */
    LCD_Fill(0,   0,   24,  4,  WHITE);      /* 左上：横 */
    LCD_Fill(0,   0,   4,   24, WHITE);      /* 左上：竖 */
    LCD_Fill(215, 0,   239, 4,  WHITE);      /* 右上：横 */
    LCD_Fill(235, 0,   239, 24, WHITE);      /* 右上：竖 */
    LCD_Fill(0,   315, 24,  319, WHITE);     /* 左下：横 */
    LCD_Fill(0,   295, 4,   319, WHITE);     /* 左下：竖 */
    LCD_Fill(215, 315, 239, 319, WHITE);     /* 右下：横 */
    LCD_Fill(235, 295, 239, 319, WHITE);     /* 右下：竖 */

    printf("[LCD] L3 三色竖条: 左RED 中GREEN 右BLUE + 四角白标\r\n");
    printf("[LCD]     字镜像/左右反 -> 改 lcd_ex.c 第58行 0x36 参数 0x48 -> 0x08\r\n");
    printf("[LCD]     仅红蓝反      -> 同一个参数再去掉 0x08（即 0x00）\r\n");
    delay_ms(1200);
}

static void test_l4_text(void)
{
    uint8_t i;

    LCD_Clear(BLACK);

    LCD_ShowString(4, 4, "L4 FONT/NUMBER", WHITE, BLACK);
    LCD_DrawLine(0, 23, 239, 23, LGRAY);

    /* --- ① 95 个可打印 ASCII 字符全家福（每行 30 个，8×16 正好铺满 240 宽）--- */
    for (i = 0; i < 95; i++)
    {
        LCD_ShowChar((uint16_t)((i % 30) * 8), (uint16_t)(32 + (i / 30) * 16),
                     (char)(0x20 + i), WHITE, BLACK);
    }

    /* --- ② 整数：len 是"最小宽度"，不足补前导空格（防显示抖动）--- */
    LCD_ShowString(4, 104, "ShowInt   25",  LGRAY, BLACK);
    LCD_ShowString(112, 104, "[", LGRAY, BLACK);
    LCD_ShowInt(120, 104, 25, 4, YELLOW, BLACK);          /* "  25" */
    LCD_ShowString(160, 104, "]", LGRAY, BLACK);

    LCD_ShowString(4, 124, "ShowInt   -7",  LGRAY, BLACK);
    LCD_ShowString(112, 124, "[", LGRAY, BLACK);
    LCD_ShowInt(120, 124, -7, 4, YELLOW, BLACK);          /* "  -7" */
    LCD_ShowString(160, 124, "]", LGRAY, BLACK);

    /* --- ③ 小数：dec 位不足补 0，intlen 补前导空格 --- */
    LCD_ShowString(4, 144, "Float  25.63",  LGRAY, BLACK);
    LCD_ShowString(112, 144, "[", LGRAY, BLACK);
    LCD_ShowFloat(120, 144, 25.63f, 3, 1, CYAN, BLACK);   /* " 25.6" */
    LCD_ShowString(160, 144, "]", LGRAY, BLACK);

    LCD_ShowString(4, 164, "Float    0.5",  LGRAY, BLACK);
    LCD_ShowString(112, 164, "[", LGRAY, BLACK);
    LCD_ShowFloat(120, 164, 0.5f, 1, 2, CYAN, BLACK);     /* "0.50"（末尾 0 必须显示）*/
    LCD_ShowString(160, 164, "]", LGRAY, BLACK);

    LCD_ShowString(4, 184, "Float  -3.14",  LGRAY, BLACK);
    LCD_ShowString(112, 184, "[", LGRAY, BLACK);
    LCD_ShowFloat(120, 184, -3.1416f, 1, 4, CYAN, BLACK); /* "-3.1416" */
    LCD_ShowString(160, 184, "]", LGRAY, BLACK);

    LCD_ShowString(4, 220, "0123456789 ABCDEFG abcdefg", LGRAY, BLACK);
    LCD_ShowString(4, 240, ".,:;/-|+()[]{}<>?!@#$%^&*", LGRAY, BLACK);
    LCD_ShowString(4, 260, "LCD ILI9341 240x320 OK", WHITE, BLACK);

    printf("[LCD] L4 字模+格式化: 看字符是否端正、末尾 0 有没有被吃掉\r\n");
    delay_ms(1800);
}

/*
   L5 mini-weather 界面预览
   这不是最终 UI，是给 TaskDisplay 的"样板 + 布局参考"：
   后面做局部刷新时，标题栏、4 行数据、趋势图、状态栏各自有固定矩形区域，
   只重画变化的那一小块（别全屏 Clear，否则肉眼看得出闪）。
    */
static void draw_weather_ui(void)
{
    /* 趋势图的折线采样点（x, y），后面接真传感器时把 y 换成换算值 */
    static const uint16_t px[8] = { 12,  42,  72, 102, 132, 162, 192, 222};
    static const uint16_t py[8] = {226, 216, 202, 212, 186, 194, 176, 184};
    uint8_t i;

    LCD_Clear(BLACK);

    /* ---- ① 标题栏：整块矩形一次填充 ---- */
    LCD_Fill(0, 0, 239, 25, COLOR_TITLE);
    LCD_ShowString(4, 5, "MiniWeather", WHITE, COLOR_TITLE);

    /* ---- ② 四行传感器数值（现在是模拟值，接上传感器后换成变量）---- */
    LCD_ShowString(8,  44, "Temp", WHITE,   BLACK);
    LCD_ShowFloat (56, 44, 25.6f, 3, 1, YELLOW, BLACK);
    LCD_ShowString(104, 44, "C",   YELLOW,  BLACK);

    LCD_ShowString(8,  68, "Humi", WHITE,   BLACK);
    LCD_ShowInt   (56, 68, 58, 3, CYAN,     BLACK);
    LCD_ShowString(88, 68, "%",    CYAN,    BLACK);

    LCD_ShowString(8,  92, "Pres", WHITE,   BLACK);
    LCD_ShowInt   (56, 92, 1013, 4, GREEN,  BLACK);
    LCD_ShowString(96, 92, "hPa",  GREEN,   BLACK);

    LCD_ShowString(8,  116, "VOC", WHITE,   BLACK);
    LCD_ShowInt   (56, 116, 42, 3, MAGENTA, BLACK);
    LCD_ShowString(88, 116, "ppb", MAGENTA, BLACK);

    /* ---- ③ 趋势图区（框内以后只做局部刷新）---- */
    LCD_DrawRect(6, 142, 233, 232, COLOR_GRID);
    LCD_DrawLine(7, 187, 232, 187, COLOR_GRID);           /* 中间参考线 */
    for (i = 0; i < 7; i++)
        LCD_DrawLine(px[i], py[i], px[i + 1], py[i + 1], RED);

    /* ---- ④ 底部状态栏 ---- */
    LCD_Fill(0, 300, 239, 319, COLOR_BAR);
    LCD_ShowString(4, 302, "WiFi -- MQTT --", LGRAY, COLOR_BAR);

    printf("[LCD] L5 界面预览已绘制（标题栏/4行数据/趋势图/状态栏）\r\n");
}

/*
   对外入口：跑完全部 5 级
    */
void LCD_Test(void)
{
    printf("\r\n");
    printf("========================================\r\n");
    printf("     LCD 上电自检  ILI9341 240x320      \r\n");
    printf("========================================\r\n");
    printf("[LCD] CS=PB12  DC=PE7  RST=PA4  BLK=PE8\r\n");
    printf("[LCD] SCK=PB13 MOSI=PB15   MISO=未接   \r\n");
    printf("[LCD] SPI2: Mode0 / 8bit / MSB / 10.5MHz\r\n");
    printf("----------------------------------------\r\n");

    test_l1_backlight();
    test_l2_colors();
    test_l3_direction();
    test_l4_text();

    printf("[LCD] L5 绘制 mini-weather 界面预览...\r\n");
    draw_weather_ui();

    printf("----------------------------------------\r\n");
    printf("[OK] 自检流程跑完，对照以下现象核对：\r\n");
    printf("     1) L1 有亮暗变化           -> PE8 背光正常\r\n");
    printf("     2) L2 五色满屏、无残缺     -> SPI/GRAM 正常\r\n");
    printf("     3) L3 左红右蓝、角标在四角 -> 方向/BGR 正常\r\n");
    printf("     4) L4 字符端正、0.50 有位  -> 字模/格式化正常\r\n");
    printf("     5) 最后停在 mini-weather 界面\r\n");
    printf("========================================\r\n\r\n");
}
