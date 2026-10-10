#ifndef __BEEP_H
#define __BEEP_H

#include "main.h"


/* BEEP_AlarmTick() 的调用周期（毫秒），必须和实际调用周期一致 */
#define BEEP_TICK_MS        10

/* 蜂鸣器操作宏：直接用 main.h 里的 BEEP_GPIO_Port / BEEP_Pin */
#define BEEP_ON()     HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_RESET)
#define BEEP_OFF()    HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_RESET)
#define BEEP_TOGGLE() HHAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, GPIO_PIN_RESET)

/* ================== 函数声明 ================== */
void    BEEP_Init(void);                /* 初始化：上电先关蜂鸣器 */

/* —— 阻塞版（裸机/测试用，RTOS 下别用）—— */
void    BEEP_Alarm(uint16_t times, uint16_t on_ms, uint16_t off_ms);

/* —— 非阻塞版（RTOS 项目用这套）—— */
void    BEEP_AlarmStart(uint16_t times, uint16_t on_ms, uint16_t off_ms);
void    BEEP_AlarmTick(void);           /* 每 BEEP_TICK_MS 毫秒调用一次 */
uint8_t BEEP_IsAlarming(void);          /* 是否正在报警（1=响着） */
void    BEEP_AlarmStop(void);           /* 立即停止报警 */

/* —— 单声提示（非阻塞）—— */
void    BEEP_Beep(uint16_t ms);

#endif /* __BEEP_H */
