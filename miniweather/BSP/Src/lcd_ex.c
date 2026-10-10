#include "lcd.h"
#include "delay.h"


void lcd_ex_ili9341_reginit(void)
{
    LCD_WriteCmd(0xCF);
    LCD_WriteData(0x00);
    LCD_WriteData(0xC1);
    LCD_WriteData(0x30);

    LCD_WriteCmd(0xED);
    LCD_WriteData(0x64);
    LCD_WriteData(0x03);
    LCD_WriteData(0x12);
    LCD_WriteData(0x81);

    LCD_WriteCmd(0xE8);
    LCD_WriteData(0x85);
    LCD_WriteData(0x10);
    LCD_WriteData(0x7A);

    LCD_WriteCmd(0xCB);
    LCD_WriteData(0x39);
    LCD_WriteData(0x2C);
    LCD_WriteData(0x00);
    LCD_WriteData(0x34);
    LCD_WriteData(0x02);

    LCD_WriteCmd(0xF7);
    LCD_WriteData(0x20);

    LCD_WriteCmd(0xEA);
    LCD_WriteData(0x00);
    LCD_WriteData(0x00);

    LCD_WriteCmd(0xC0);    /* Power control */
    LCD_WriteData(0x1B);   /* VRH[5:0] */

    LCD_WriteCmd(0xC1);    /* Power control */
    LCD_WriteData(0x01);   /* SAP[2:0];BT[3:0] */

    LCD_WriteCmd(0xC5);    /* VCM control */
    LCD_WriteData(0x30);
    LCD_WriteData(0x30);

    LCD_WriteCmd(0xC7);    /* VCM control2 */
    LCD_WriteData(0xB7);

    LCD_WriteCmd(0x36);    /* Memory Access Control */
    LCD_WriteData(0x08);   /* 横屏从左到右，从上到下 */

    LCD_WriteCmd(0x3A);    /* 像素格式 */
    LCD_WriteData(0x55);   /* 16bit RGB565 */

    LCD_WriteCmd(0xB1);
    LCD_WriteData(0x00);
    LCD_WriteData(0x1A);

    LCD_WriteCmd(0xB6);    /* Display Function Control */
    LCD_WriteData(0x0A);
    LCD_WriteData(0xA2);

    LCD_WriteCmd(0xF2);    /* 3Gamma Function Disable */
    LCD_WriteData(0x00);

    LCD_WriteCmd(0x26);    /* Gamma curve selected */
    LCD_WriteData(0x01);

    LCD_WriteCmd(0xE0);    /* Set Gamma */
    LCD_WriteData(0x0F);
    LCD_WriteData(0x2A);
    LCD_WriteData(0x28);
    LCD_WriteData(0x08);
    LCD_WriteData(0x0E);
    LCD_WriteData(0x08);
    LCD_WriteData(0x54);
    LCD_WriteData(0xA9);
    LCD_WriteData(0x43);
    LCD_WriteData(0x0A);
    LCD_WriteData(0x0F);
    LCD_WriteData(0x00);
    LCD_WriteData(0x00);
    LCD_WriteData(0x00);
    LCD_WriteData(0x00);

    LCD_WriteCmd(0xE1);    /* Set Gamma */
    LCD_WriteData(0x00);
    LCD_WriteData(0x15);
    LCD_WriteData(0x17);
    LCD_WriteData(0x07);
    LCD_WriteData(0x11);
    LCD_WriteData(0x06);
    LCD_WriteData(0x2B);
    LCD_WriteData(0x56);
    LCD_WriteData(0x3C);
    LCD_WriteData(0x05);
    LCD_WriteData(0x10);
    LCD_WriteData(0x0F);
    LCD_WriteData(0x3F);
    LCD_WriteData(0x3F);
    LCD_WriteData(0x0F);

    LCD_WriteCmd(0x2B);    /* 行地址 0~319 */
    LCD_WriteData(0x00);
    LCD_WriteData(0x00);
    LCD_WriteData(0x01);
    LCD_WriteData(0x3F);

    LCD_WriteCmd(0x2A);    /* 列地址 0~239 */
    LCD_WriteData(0x00);
    LCD_WriteData(0x00);
    LCD_WriteData(0x00);
    LCD_WriteData(0xEF);

    LCD_WriteCmd(0x11);    /* Exit Sleep */
    delay_ms(120);

    LCD_WriteCmd(0x29);    /* Display ON */
}
