#ifndef __KEY_H
#define __KEY_H

#include "main.h"


/* 读取按键电平 */
#define KEY0_STATE()   HAL_GPIO_ReadPin(KEY0_GPIO_Port, KEY0_Pin)
#define KEY1_STATE()   HAL_GPIO_ReadPin(KEY1_GPIO_Port, KEY1_Pin)
#define KEY2_STATE()   HAL_GPIO_ReadPin(KEY2_GPIO_Port, KEY2_Pin)
#define KEY_UP_STATE() HAL_GPIO_ReadPin(KEY_UP_GPIO_Port, KEY_UP_Pin)

/* 按键编号（返回值） */
#define KEY0_PRESS   0
#define KEY1_PRESS   1
#define KEY2_PRESS   2
#define KEYUP_PRESS  3
#define KEY_NONE     0xFF   /* 没有按键 */

/* ================== 函数声明 ================== */
void    KEY_Init(void);          /* 初始化按键 GPIO */
uint8_t KEY_Scan(uint8_t mode);  /* 扫描按键，返回按下的按键编号 */
                                  /* mode=0 不支持连按，mode=1 支持连按 */

#endif /* __KEY_H */
