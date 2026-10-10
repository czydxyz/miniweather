/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
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

/* Includes ------------------------------------------------------------------*/
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins as
        * Analog
        * Input
        * Output
        * EVENT_OUT
        * EXTI
*/
/* ========== GPIO 初始化 ==========
   这是所有外设的基础，必须第一个初始化
   引脚一览：
     PF9  = LED0（低电平亮）      PF10 = LED1（低电亮）
     PE4  = KEY0（上拉，按下接地）  PE3  = KEY1    PE2  = KEY2
     PA0  = KEY_UP（下拉，按下接 3.3V）
     PF8  = BEEP（高电平响）
     PE7  = LCD_DC（数据/命令选择，0=命令 1=数据）
     PE8  = LCD_BLK（背光，高电平亮）
     PB12 = LCD_CS（LCD 片选，低有效）
     PB14 = FLASH_CS（W25Q128 片选，低有效）
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /* 初始电平设置：LED/蜂鸣器/LCD 控制脚全部拉低
     LED 低电平亮 → 初始灭；蜂鸣器低电平静音 → 初始不响 */
  HAL_GPIO_WritePin(GPIOF, BEEP_Pin|LED0_Pin|LED1_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOE, LCD_DC_Pin|LCD_BLK_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, LCD_CS_Pin|FLASH_CS_Pin, GPIO_PIN_RESET);

  /* 按键 PE2/PE3/PE4：上拉输入模式
     上拉 = 内部接 3.3V，按下时接地为低电平 */
  GPIO_InitStruct.Pin = KEY2_Pin|KEY1_Pin|KEY0_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /* LED0/LED1 + 蜂鸣器：推挽输出，低速（LED 不需要高频） */
  GPIO_InitStruct.Pin = BEEP_Pin|LED0_Pin|LED1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOF, &GPIO_InitStruct);

  /* KEY_UP（PA0）：下拉输入，按下接 3.3V 为高电平 */
  GPIO_InitStruct.Pin = KEY_UP_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(KEY_UP_GPIO_Port, &GPIO_InitStruct);

  /* LCD_DC/LCD_BLK：推挽输出，高速（SPI 通信时序要求快速切换） */
  GPIO_InitStruct.Pin = LCD_DC_Pin|LCD_BLK_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /* LCD_CS/FLASH_CS：推挽输出，高速（SPI 片选需要快速切换） */
  GPIO_InitStruct.Pin = LCD_CS_Pin|FLASH_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */
