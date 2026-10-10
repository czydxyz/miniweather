#ifndef __LCD_H
#define __LCD_H

#include "main.h"
#include "spi.h"

#define LCD_W   240
#define LCD_H   320


/* LCD CS：低有效，选中 LCD */
#define LCD_CS_L()   HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET)
#define LCD_CS_H()   HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET)

/* DC：0=命令，1=数据 */
#define LCD_DC_L()   HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET)
#define LCD_DC_H()   HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET)

/* 背光（高电平亮） */
#define LCD_BL_ON()  HAL_GPIO_WritePin(LCD_BLK_GPIO_Port, LCD_BLK_Pin, GPIO_PIN_SET)
#define LCD_BL_OFF() HAL_GPIO_WritePin(LCD_BLK_GPIO_Port, LCD_BLK_Pin, GPIO_PIN_RESET)

/* 硬件复位（PA4，低电平复位） */
#define LCD_RST_L()  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET)
#define LCD_RST_H()  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET)

/* Flash CS（挂在 SPI1，但要保证空闲时为高，不干扰总线） */
#define FLASH_CS_H() HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET)

/* ================== 颜色定义（RGB565）================== */
#define WHITE    0xFFFF
#define BLACK    0x0000
#define RED      0xF800
#define GREEN    0x07E0
#define BLUE     0x001F
#define YELLOW   0xFFE0
#define CYAN     0x07FF
#define MAGENTA  0xF81F
#define BROWN    0xBC40
#define GRAY     0x8430
#define BRRED    0xFC07
#define LGRAY    0xC618

/* ================== 函数声明 ================== */

/* ---- 底层 3 函数 ---- */
void LCD_WriteCmd(uint8_t cmd);          /* DC=0，发 1 字节命令 */
void LCD_WriteData(uint8_t data);        /* DC=1，发 1 字节参数 */
void LCD_WriteData16(uint16_t data);     /* DC=1，发 2 字节像素（高字节在前） */

/* ---- 初始化 ---- */
void LCD_Init(void);

/* ---- 核心：设窗口 ---- */
void LCD_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

/* ---- 基本绘图 ---- */
void LCD_Fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void LCD_Clear(uint16_t color);
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void LCD_DrawRect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);

/* ---- 文字 / 数字（8×16 字体）---- */
void LCD_ShowChar  (uint16_t x, uint16_t y, char ch, uint16_t fc, uint16_t bc);
void LCD_ShowString(uint16_t x, uint16_t y, const char *str, uint16_t fc, uint16_t bc);
void LCD_ShowInt   (uint16_t x, uint16_t y, int32_t num, uint8_t len, uint16_t fc, uint16_t bc);
void LCD_ShowFloat (uint16_t x, uint16_t y, float val, uint8_t intlen, uint8_t dec,
                    uint16_t fc, uint16_t bc);

/* ---- 自检例程不在这里 ----
   上电自检/界面预览在独立的 lcd_test.h + lcd_test.c 里，
   驱动文件保持"纯驱动"，不需要自检时直接从工程 Remove 掉那个 .c 即可。*/

#endif /* __LCD_H */
