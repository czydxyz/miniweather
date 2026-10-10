#ifndef __LED_H
#define __LED_H

#include "main.h"


/* LED 操作宏：直接用 main.h 里的 LED0_GPIO_Port / LED0_Pin 等 */
#define LED0_ON()     HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET)
#define LED0_OFF()    HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET)
#define LED0_TOGGLE() HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin)

#define LED1_ON()     HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET)
#define LED1_OFF()    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET)
#define LED1_TOGGLE() HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin)

/* ================== 函数声明 ================== */
void LED_Init(void);     /* 初始化 LED GPIO（CubeMX 已配，这里可空实现或补充） */

#endif /* __LED_H */
