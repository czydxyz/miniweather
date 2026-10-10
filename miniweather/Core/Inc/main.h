/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* ------------------------------------------------------------------
   独立看门狗 IWDG（2026-09-29 启用）
   ------------------------------------------------------------------
   为什么在这里声明成 extern：
     comm.c 在擦 Flash / 切 Bank 之前要手动喂一次狗（那两步会阻塞
     几百毫秒到 2 秒，空闲任务根本跑不到），所以它得能拿到 hiwdg。
   超时时间见 main.c 里的 MX_IWDG_Init()。
   ------------------------------------------------------------------ */
extern IWDG_HandleTypeDef hiwdg;
void   MX_IWDG_Init(void);
void   IWDG_Feed(void);          /* 喂狗（空闲任务 + 长阻塞路径都调它） */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define KEY2_Pin GPIO_PIN_2
#define KEY2_GPIO_Port GPIOE
#define KEY1_Pin GPIO_PIN_3
#define KEY1_GPIO_Port GPIOE
#define KEY0_Pin GPIO_PIN_4
#define KEY0_GPIO_Port GPIOE
#define BEEP_Pin GPIO_PIN_8
#define BEEP_GPIO_Port GPIOF
#define LED0_Pin GPIO_PIN_9
#define LED0_GPIO_Port GPIOF
#define LED1_Pin GPIO_PIN_10
#define LED1_GPIO_Port GPIOF
#define KEY_UP_Pin GPIO_PIN_0
#define KEY_UP_GPIO_Port GPIOA
#define LCD_DC_Pin GPIO_PIN_7
#define LCD_DC_GPIO_Port GPIOE
#define LCD_BLK_Pin GPIO_PIN_8
#define LCD_BLK_GPIO_Port GPIOE
#define LCD_CS_Pin GPIO_PIN_12
#define LCD_CS_GPIO_Port GPIOB
#define FLASH_CS_Pin GPIO_PIN_14
#define FLASH_CS_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* ==========================================================================
 * APP_WITH_BOOTLOADER —— App 是否搭配 Bootloader 运行
 *
 *   0 = 现在这样：App 独占 Flash，链接在 0x08000000，
 *       用 ST-Link 烧进去直接就能跑（的调试方式，不动它）
 *   1 = 搭配 Bootloader：App 必须链接在 0x08010000，
 *       并且 Keil 的 IROM1 要改成 Start=0x8010000 / Size=0x50000
 *
 * ⚠️ 两个必须配套改，改一个不改另一个必出问题：
 *     宏改 1 但 IROM 没改  -> App 烧在 0x08000000，VTOR 却指向 0x08010000，一开中断就飞
 *     IROM 改了但宏还是 0  -> App 在 0x08010000 能跑主循环，中断全去 Bootloader 的表里找
 * ========================================================================*/
#ifndef APP_WITH_BOOTLOADER
#define APP_WITH_BOOTLOADER     1
#endif

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
