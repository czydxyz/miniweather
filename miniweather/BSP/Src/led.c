#include "led.h"

void LED_Init(void)
{
    LED0_OFF();      /* 上电默认灭，由程序逻辑决定什么时候亮 */
    LED1_OFF();
}
