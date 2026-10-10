#include "key.h"
#include "delay.h"


/* 初始化：CubeMX 已经把 4 个脚配好输入模式，这里只需保证状态干净 */
void KEY_Init(void)
{
    /* 引脚模式由 CubeMX 的 MX_GPIO_Init() 配置，本函数不做 GPIO 改动。
       保留它是为了和 LED_Init / BEEP_Init 风格一致，也方便以后加外部中断 */
}

uint8_t KEY_Scan(uint8_t mode)
{
    static uint8_t key_up = 1;      /* 1 = 上次是松开状态 */
    uint8_t        pressed;         /* 本次是否有键按下 */

    if (mode) key_up = 1;           /* 连按模式：每轮都强制当作"刚松开" */

    pressed = (KEY0_STATE()   == GPIO_PIN_RESET) ||
              (KEY1_STATE()   == GPIO_PIN_RESET) ||
              (KEY2_STATE()   == GPIO_PIN_RESET) ||
              (KEY_UP_STATE() == GPIO_PIN_SET);

    if (key_up && pressed)
    {
        delay_ms(10);               /* 消抖：等抖动过去再读一次 */
        key_up = 0;                 /* 标记已处理，防止按住期间重复触发 */

        if      (KEY0_STATE()   == GPIO_PIN_RESET) return KEY0_PRESS;
        else if (KEY1_STATE()   == GPIO_PIN_RESET) return KEY1_PRESS;
        else if (KEY2_STATE()   == GPIO_PIN_RESET) return KEY2_PRESS;
        else if (KEY_UP_STATE() == GPIO_PIN_SET)   return KEYUP_PRESS;
    }
    else if (!pressed)
    {
        key_up = 1;                 /* 全部松开，恢复可触发状态 */
    }

    return KEY_NONE;
}
