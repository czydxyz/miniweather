/* ILI9341 LCD 驱动（SPI2：CS=PB12 DC=PE7 BL=PE8 RST=PA4）
 * [约束] PA4(RST) 未在 CubeMX 的 gpio.c 中配置，本文件自配推挽输出，勿依赖 CubeMX。
 * [约束] 命令/数据由 DC 电平决定，每次传输必须「先切 DC，再拉 CS」，顺序反了会写错。
 * [约束] 坐标 >255，必须拆高/低字节：0x2A 列地址、0x2B 行地址各 4 个参数。
 * [约束] 字模 asc2_1608 为逐列式，需开「宽1 高16」细窗口逐列刷 8 次，不可开 8x16 大窗口。
 * [约束] MADCTL(0x36) = 0x08（0x48 会左右镜像）。
 * [约束] 批量写像素分包上限见 LCD_BURST_MAX，全屏 240*320*2=153600B 一次传会溢出。
 */
#include "lcd.h"
#include "lcdfont.h"
#include "delay.h"

/* SPI2 句柄（CubeMX 生成在 spi.c 里） */
extern SPI_HandleTypeDef hspi2;

/* lcd_ex.c 里定义，初始化命令序列 */
extern void lcd_ex_ili9341_reginit(void);

/*
   内部常量 / 私有工具
    */

#define LCD_SPI_TIMEOUT     100     /* HAL_SPI_Transmit 超时，单位 ms */

#define LCD_PIX_PER_BURST   32
static uint8_t s_burst[LCD_PIX_PER_BURST * 2];

/*
   私有：SPI 发送 n 字节（自动处理 >65535 的分包）
   p 必须是有效指针；len 允许任意长度
    */
static void LCD_SPI_Send(const uint8_t *p, uint32_t len)
{
    while (len > 0)
    {
        /* Size 是 uint16_t，单包上限 65535 */
        uint16_t chunk = (len > 65535U) ? (uint16_t)65535 : (uint16_t)len;

        HAL_SPI_Transmit(&hspi2, (uint8_t *)p, chunk, LCD_SPI_TIMEOUT);

        p   += chunk;
        len -= chunk;
    }
}

static void LCD_RstPinInit(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();          /* 开 GPIOA 时钟 */

    gpio.Pin   = GPIO_PIN_4;               /* PA4 = LCD_RST */
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;      /* 推挽输出 */
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);   /* 空闲态：不复位（高） */
}

static void LCD_WriteColorBurst(uint16_t color, uint32_t n)
{
    uint32_t i;

    if (n == 0) return;

    /* 先用固定颜色把 burst 缓冲填满 */
    for (i = 0; i < LCD_PIX_PER_BURST; i++)
    {
        s_burst[i * 2    ] = (uint8_t)(color >> 8);   /* RGB565 高字节（R+G 高位） */
        s_burst[i * 2 + 1] = (uint8_t)(color & 0xFF); /* RGB565 低字节（G 低位+B） */
    }

    LCD_DC_H();                 /* 数据模式（整段只切这一次） */
    LCD_CS_L();                 /* 选中 LCD（整段只拉这一次） */

    while (n > 0)
    {
        uint32_t chunk = (n > LCD_PIX_PER_BURST) ? LCD_PIX_PER_BURST : n;
        LCD_SPI_Send(s_burst, chunk * 2);
        n -= chunk;
    }

    /* 注意顺序：必须先等 SPI 真正发完（上面的 HAL_SPI_Transmit 是阻塞的，
       已经保证最后一个字节移位出去了），再拉高 CS。 */
    LCD_CS_H();
}


void LCD_WriteCmd(uint8_t cmd)
{
    LCD_DC_L();                                  /* DC=0：接下来这个字节是命令 */
    LCD_CS_L();                                  /* CS=0：选中 LCD */
    LCD_SPI_Send(&cmd, 1);
    LCD_CS_H();                                  /* CS=1：结束这次传输 */
}

void LCD_WriteData(uint8_t data)
{
    LCD_DC_H();                                  /* DC=1：接下来这个字节是数据 */
    LCD_CS_L();
    LCD_SPI_Send(&data, 1);
    LCD_CS_H();
}

void LCD_WriteData16(uint16_t data)
{
    uint8_t buf[2];

    buf[0] = (uint8_t)(data >> 8);               /* RGB565 高字节先发（大端） */
    buf[1] = (uint8_t)(data & 0xFF);

    LCD_DC_H();
    LCD_CS_L();
    LCD_SPI_Send(buf, 2);                        /* 一次发 2 字节，中途不能拉抬 CS */
    LCD_CS_H();
}


/*
   初始化
    */

void LCD_Init(void)
{
    /* 0. RST 引脚自己初始化（CubeMX 目前没配 PA4，详见 LCD_RstPinInit 注释） */
    LCD_RstPinInit();

    LCD_CS_H();
    FLASH_CS_H();

    /* 2. 硬件复位
          RESX 拉低 ≥10us（这里给 15ms，充分），内部寄存器全部回到默认值。
          上电后不复位，ILI9341 不会响应任何命令。 */
    LCD_RST_L();
    delay_ms(15);
    LCD_RST_H();
    delay_ms(15);        /* 复位释放后要等内部振荡器/电荷泵稳定（手册要求 ≥5ms） */

    /* 3. 关背光初始化（避免清屏前的随机画面闪一下），初始化完再开 */
    LCD_BL_OFF();

    /* 4. 发 ILI9341 初始化命令序列（lcd_ex.c，约 20 条命令 + Gamma 表） */
    lcd_ex_ili9341_reginit();

    /* 5. 开背光 */
    LCD_BL_ON();

    /* 6. 清屏（顺带验证整个链路通不通：亮了黑屏 = 成功） */
    LCD_Clear(BLACK);
}


void LCD_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    /* 列(X)地址 */
    LCD_WriteCmd(0x2A);
    LCD_WriteData((uint8_t)(x1 >> 8));     /* 起始 X 高字节（240 以内永远是 0） */
    LCD_WriteData((uint8_t)(x1 & 0xFF));   /* 起始 X 低字节 */
    LCD_WriteData((uint8_t)(x2 >> 8));     /* 结束 X 高字节 */
    LCD_WriteData((uint8_t)(x2 & 0xFF));   /* 结束 X 低字节 */

    /* 行(Y)地址 —— 这里原来漏了 4 个参数字节，直接跳到 0x2B 命令本身，
       导致所有窗口都只有 1 像素高 / 地址错乱 */
    LCD_WriteCmd(0x2B);
    LCD_WriteData((uint8_t)(y1 >> 8));     /* 起始 Y 高字节（320 要用 9bit） */
    LCD_WriteData((uint8_t)(y1 & 0xFF));   /* 起始 Y 低字节 */
    LCD_WriteData((uint8_t)(y2 >> 8));     /* 结束 Y 高字节 */
    LCD_WriteData((uint8_t)(y2 & 0xFF));   /* 结束 Y 低字节 */

    /* 开始写 GRAM：从此刻起发的每个字节都被当成像素，直到下一条命令到来 */
    LCD_WriteCmd(0x2C);
}

/*
   填充矩形
   - 设窗口 → 连发 (x2-x1+1)*(y2-y1+1) 个像素颜色
   - 用 burst 版批量写，比一个个 WriteData16 快很多倍
    */

void LCD_Fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    uint32_t total;

    /* 非法参数直接返回 */
    if (x1 > x2 || y1 > y2) return;
    if (x1 >= LCD_W || y1 >= LCD_H) return;

    /* 超出屏幕部分裁剪掉：
       不裁剪的话 SetWindow 会把 AC 推到 GRAM 外，
       写进去的数据落到别的地方，下次开窗就串台。 */
    if (x2 >= LCD_W) x2 = LCD_W - 1;
    if (y2 >= LCD_H) y2 = LCD_H - 1;

    total = (uint32_t)(x2 - x1 + 1) * (uint32_t)(y2 - y1 + 1);

    LCD_SetWindow(x1, y1, x2, y2);
    LCD_WriteColorBurst(color, total);
}

/*
   清屏 = 全屏填充
    */

void LCD_Clear(uint16_t color)
{
    LCD_Fill(0, 0, LCD_W - 1, LCD_H - 1, color);
}

/*
   画点 = 1×1 窗口 + 写 1 个像素
   （慢：每画一个点要重新发 10 个参数字节；画大量点请用 LCD_Fill/画线）
    */

void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= LCD_W || y >= LCD_H) return;     /* 越界保护 */

    LCD_SetWindow(x, y, x, y);                /* 窗口起点 = 终点 = 1 个格子 */
    LCD_WriteData16(color);                   /* 只写 1 个像素 */
}

/*
   画线（Bresenham 算法）
   - 水平/垂直的退化情况直接用 LCD_Fill 填，快得多
   - 斜线用整数增量判定，全程无浮点
    */

void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    int16_t dx, dy;
    int16_t incx, incy;
    int16_t err;
    uint16_t swap;
    uint16_t x, y;
    uint16_t i, steps;

    if (x1 >= LCD_W || x2 >= LCD_W || y1 >= LCD_H || y2 >= LCD_H) return;

    /* 水平线：直接填充一条 1 像素高的矩形 */
    if (y1 == y2)
    {
        if (x1 > x2) { swap = x1; x1 = x2; x2 = swap; }
        LCD_Fill(x1, y1, x2, y2, color);
        return;
    }

    /* 垂直线：直接填充一条 1 像素宽的矩形 */
    if (x1 == x2)
    {
        if (y1 > y2) { swap = y1; y1 = y2; y2 = swap; }
        LCD_Fill(x1, y1, x2, y2, color);
        return;
    }

    /* ---- 斜线：布雷森汉姆 ----
       核心思想：不走"斜率浮点计算"，而是维护一个误差累计值 err。
       每走一步主方向，err 累加次方向的增量；当 err 超过半个格子的阈值，
       次方向也走一步并回退一个主方向步长。全程整数。 */
    dx    = (int16_t)x2 - (int16_t)x1;
    dy    = (int16_t)y2 - (int16_t)y1;
    incx  = (dx > 0) ? 1 : -1;
    incy  = (dy > 0) ? 1 : -1;
    dx    = (dx > 0) ? dx : -dx;
    dy    = (dy > 0) ? dy : -dy;

    /* 注意这里统一用 x = (uint16_t)(x + incx) 的形式：
       incx 为 -1 时表达式先在 int 里算完再截断回 uint16_t 回绕，等价减 1。
       千万别写成 x += (uint16_t)incx —— 那样 -1 会先变成 65535 再加，
       虽然结果碰巧也对，但阅读起来是个坑。 */
    x = x1;
    y = y1;
    steps = (dx > dy) ? (uint16_t)dx : (uint16_t)dy;
    err   = 0;

    for (i = 0; i <= steps; i++)
    {
        LCD_DrawPoint(x, y, color);

        if (dx > dy)                /* X 是主方向，Y 是次方向 */
        {
            x = (uint16_t)(x + incx);
            err += dy;
            if (err >= dx) { err -= dx; y = (uint16_t)(y + incy); }
        }
        else                        /* Y 是主方向，X 是次方向 */
        {
            y = (uint16_t)(y + incy);
            err += dx;
            if (err >= dy) { err -= dy; x = (uint16_t)(x + incx); }
        }
    }
}

/*
   画矩形（空心）= 4 条边
    */

void LCD_DrawRect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    if (x1 > x2) { uint16_t t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { uint16_t t = y1; y1 = y2; y2 = t; }

    LCD_Fill(x1, y1, x2, y1, color);        /* 上边 */
    LCD_Fill(x1, y2, x2, y2, color);        /* 下边 */
    LCD_Fill(x1, y1, x1, y2, color);        /* 左边 */
    LCD_Fill(x2, y1, x2, y2, color);        /* 右边 */
}


void LCD_ShowChar(uint16_t x, uint16_t y, char ch, uint16_t fc, uint16_t bc)
{
    uint8_t col, seg, row;
    uint8_t temp;
    const uint8_t *font;

    /* 越界保护：8×16 的字符必须整个落在屏幕内 */
    if (x + 7 >= LCD_W || y + 15 >= LCD_H) return;

    /* 字模只覆盖 ' '(0x20) ~ '~'(0x7E)。
       这个判断绝对不能删：以后如果传进来一个中文字符的高字节，
       ch 会被当成负数/大数，(ch - ' ') 算出巨大下标读到数组外 → HardFault。 */
    if (ch < ' ' || ch > '~') return;

    /* 定位字模：
       asc2_1608 定义是 const unsigned char asc2_1608[95][16]
       asc2_1608[n]  是一个含 16 个元素的数组的【首地址类型】，
       必须再取 [0] 的元素地址才能赋给 uint8_t*。 */
    font = &asc2_1608[(uint8_t)(ch - ' ')][0];

    for (col = 0; col < 8; col++)                       /* 8 列，从左到右 */
    {
        /* 开一个宽 1、高 16 的窗口：
           起点列 = 终点列 = x+col（只有一个像素宽）
           起点行 = y，终点行 = y+15 */
        LCD_SetWindow(x + col, y, x + col, y + 15);

        for (seg = 0; seg < 2; seg++)                   /* 每列 2 字节：上 8 行 → 下 8 行 */
        {
            temp = font[col * 2 + seg];

            for (row = 0; row < 8; row++)               /* 一个字节竖向展开 8 个像素 */
            {
                if (temp & 0x80)                        /* bit7 = 这 8 行里最上面那个像素 */
                    LCD_WriteData16(fc);                /*  1 → 前景色（字的颜色） */
                else
                    LCD_WriteData16(bc);                /*  0 → 背景色（底色） */

                temp <<= 1;                             /* 左移，次高位移到最高位（往下走一行） */
            }
        }
    }
}

/*
   显示字符串
   - 循环调 LCD_ShowChar，每次 x += 8（字符宽固定 8 像素）
   - 不自动换行，超出屏幕宽度由 LCD_ShowChar 内部的越界保护拦下来
    */

void LCD_ShowString(uint16_t x, uint16_t y, const char *str, uint16_t fc, uint16_t bc)
{
    if (str == 0) return;               /* 空指针保护 */

    while (*str)
    {
        /* '\n' 手动支持换行（行高 = 字体高 16） */
        if (*str == '\n')
        {
            y += 16;
            x  = 0;
            str++;
            continue;
        }

        LCD_ShowChar(x, y, *str, fc, bc);
        x += 8;
        str++;
    }
}

/*
   显示整数
   len = 最小显示宽度，不足在高位补空格（0 = 不补，紧凑显示）
   例：LCD_ShowInt(10,20, -25, 4, WHITE, BLACK) → " -25"（前面 1 个空格）
   完全不用 sprintf（省掉 <stdio.h> 和 MicroLIB 依赖，也不会有几 KB 的代码膨胀）
    */

void LCD_ShowInt(uint16_t x, uint16_t y, int32_t num, uint8_t len, uint16_t fc, uint16_t bc)
{
    char    buf[12];                    /* "-2147483648" 最长 11 字符 + 结束符 */
    uint8_t i;
    int32_t n   = num;
    uint8_t neg = 0;

    if (len > 10) len = 10;

    if (n < 0) { neg = 1; n = -n; }     /* 注意：-2147483648 取负会溢出，工程上够用 */

    /* 从缓冲区尾部往前填：先放个位，再十位、百位…… */
    buf[11] = '\0';
    i = 11;
    do
    {
        buf[--i] = (char)('0' + (uint8_t)(n % 10));
        n /= 10;
    } while (n != 0 && i > 0);

    if (neg && i > 0) buf[--i] = '-';

    /* 高位补空格，凑够 len 宽度（11 - i 是当前实际字符数） */
    while ((uint8_t)(11u - i) < len && i > 0)
        buf[--i] = ' ';

    LCD_ShowString(x, y, &buf[i], fc, bc);
}

/*
   显示小数
   intlen = 整数部分最小宽度（不足补前导空格）
   dec    = 小数位数（0~6，不足补 0）
   例：LCD_ShowFloat(10,20, 25.63f, 2, 1, WHITE, BLACK) → "25.6"
    */

void LCD_ShowFloat(uint16_t x, uint16_t y, float val, uint8_t intlen, uint8_t dec,
                   uint16_t fc, uint16_t bc)
{
    char     buf[16];
    uint8_t  i, k;
    uint32_t scale = 1;
    int32_t  v;
    uint8_t  neg   = 0;

    if (dec > 6) dec = 6;

    for (k = 0; k < dec; k++) scale *= 10;

    /* 放大 scale 倍后四舍五入取整 → 把浮点转成整数再做十进制拆分 */
    if (val < 0.0f) { neg = 1; v = (int32_t)(-val * (float)scale + 0.5f); }
    else            {          v = (int32_t)( val * (float)scale + 0.5f); }

    buf[15] = '\0';
    i = 15;

    /* 小数部分：即使末尾是 0 也要输出（所以是先取模再整除，配合 dec 次固定循环） */
    for (k = 0; k < dec; k++)
    {
        buf[--i] = (char)('0' + (uint8_t)(v % 10));
        v /= 10;
    }

    if (dec > 0)
        buf[--i] = '.';                 /* 小数点 */

    /* 整数部分 */
    do
    {
        buf[--i] = (char)('0' + (uint8_t)(v % 10));
        v /= 10;
    } while (v != 0 && i > 0);

    if (neg && i > 0) buf[--i] = '-';

    /* 补前导空格，凑够 intlen 位整数宽度 */
    while ((uint8_t)(15u - i) < (uint8_t)(intlen + (dec ? (dec + 1) : 0)) && i > 0)
        buf[--i] = ' ';

    LCD_ShowString(x, y, &buf[i], fc, bc);
}

