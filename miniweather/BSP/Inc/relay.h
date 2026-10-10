#ifndef __RELAY_H
#define __RELAY_H

#include "main.h"


/* 1 = 高电平吸合，0 = 低电平吸合（大多数便宜模块是低电平触发） */
#define RELAY_ACTIVE_HIGH       0

/* 如果 CubeMX 里还没配 PE5/RELAY，就用这个兜底定义，避免编译报错。
   但真正能用还必须在 CubeMX 里配引脚（否则 GPIO 时钟没开，跑起来是死的）。*/
#ifndef RELAY_Pin
#define RELAY_Pin               GPIO_PIN_5
#define RELAY_GPIO_Port         GPIOE
#endif

/* ================== 操作宏 ================== */
#if RELAY_ACTIVE_HIGH
#define RELAY_ON()   HAL_GPIO_WritePin(RELAY_GPIO_Port, RELAY_Pin, GPIO_PIN_SET)
#define RELAY_OFF()  HAL_GPIO_WritePin(RELAY_GPIO_Port, RELAY_Pin, GPIO_PIN_RESET)
#else
#define RELAY_ON()   HAL_GPIO_WritePin(RELAY_GPIO_Port, RELAY_Pin, GPIO_PIN_RESET)
#define RELAY_OFF()  HAL_GPIO_WritePin(RELAY_GPIO_Port, RELAY_Pin, GPIO_PIN_SET)
#endif

/* ================== 函数声明 ================== */
void    RELAY_Init(void);               /* 初始化：默认断开 */
void    RELAY_Set(uint8_t on);          /* 1=吸合 0=断开 */
void    RELAY_Toggle(void);             /* 翻转 */
uint8_t RELAY_GetState(void);           /* 读当前状态（1=吸合） */

#endif /* __RELAY_H */
