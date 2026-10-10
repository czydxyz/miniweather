/* [约束] IWDG：LSI / Prescaler 64 / Reload 2000 约 4.0s，必须在 osKernelStart() 之前启动。
 * [约束] 搭配 Bootloader 时必须 SCB->VTOR = 0x08010000，并在开头 __enable_irq()。
 * [约束] 时钟：HSE 8MHz -> PLLM 8 / PLLN 336 / PLLP 2 -> SYSCLK 168MHz，
 *        HCLK 168MHz，APB1 42MHz，APB2 84MHz。
 * [约束] 顺序：delay_init -> LCD_Init；ESP8266_Init() -> Comm_Init()。
 * [约束] ESP8266_BootCapture() 必须尽早调用（模块上电约 300ms 吐完 bootlog）。
 * [约束] USART1(CH340) 仅用于 printf，自定义协议跑在 USART3，发错口必然超时。
 */
/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"          // HAL 库主头文件，所有 HAL 函数的入口
#include "cmsis_os.h"      // FreeRTOS CMSIS 封装层，提供 osThread/osDelay 等 API
#include "dma.h"           // DMA 配置（USART3_RX 用的 DMA1_Stream1）
#include "i2c.h"           // I2C1 配置（PB8/PB9，接 24C02 + QMA6100P）
#include "lwip.h"          // lwIP TCP/IP 协议栈（以太网通信用）
#include "spi.h"           // SPI1(W25Q128) + SPI2(ILI9341 LCD) 配置
#include "tim.h"           // TIM3 PWM 配置（PA6，舵机控制 50Hz）
#include "usart.h"         // USART1(调试串口) + USART3(ESP8266 AT 通信) 配置
#include "gpio.h"          // 所有 GPIO 引脚配置（LED/按键/蜂鸣器/LCD CS/Flash CS）

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "delay.h"
#include "lcd.h"            /* LCD 驱动（LCD_Init / LCD_Clear / ...）*/
#include "lcd_test.h"       /* LCD 上电自检 + mini-weather 界面预览 */
#include "shtc3.h"          /* SHTC3 温湿度 */
#include "bmp280.h"         /* BMP280 / BME280 气压 */
#include "sgp30.h"          /* SGP30 CO2 当量 + TVOC */
#include "servo.h"          /* 舵机（TIM3_CH1 = PA6）*/
#include "beep.h"           /* 板载蜂鸣器（PF8）*/
#include "relay.h"          /* 继电器（PE5）*/
#include "key.h"            /* 4 个板载按键 */
#include "led.h"            /* LED0/LED1 */
#include "w25q128.h"        /* 板载 SPI Flash（SPI1）*/
#include "esp8266.h"        /* ESP8266 Wi-Fi（USART3，AT 指令）*/
#include "app.h"            /* 应用层：上电自检 + 界面 + 按键 + 自动控制 */
#include "comm.h"           /* 自定义协议落地层（USART3 + 协议 + OTA）*/
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* 独立看门狗句柄（定义在这里，main.h 里 extern 出去给 comm.c 用） */
IWDG_HandleTypeDef hiwdg;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* IWDG 初始化（手写，没走 CubeMX，因为 CubeMX 会在 SystemCoreClock
   之后、用户代码之前插入它，那时传感器还没初始化完，容易误判超时） */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

#ifndef APP_BOOT_TRACE
#define APP_BOOT_TRACE          1
#endif

#if (APP_WITH_BOOTLOADER != 0) && (APP_BOOT_TRACE != 0)
/* 往 USART1 的发送数据寄存器塞一个字符，然后等它发完 */
#define BOOT_TRACE(c)  do {                          \
        USART1->DR = (uint16_t)(uint8_t)(c);         \
        while ((USART1->SR & USART_SR_TC) == 0u) { } \
    } while (0)
#else
#define BOOT_TRACE(c)  ((void)0)
#endif

/*
   原 AT24C02 读写测试已删除（24C02 那一步已经验过了）。
   需要时从《第3节-LCD接线与上电自检.md》第八节附录里粘回来即可。
   这次换成 LCD 上电自检：实际代码在 BSP/Src/lcd_test.c。
    */

void MX_IWDG_Init(void)
{
    hiwdg.Instance       = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
    hiwdg.Init.Reload    = 2000u;              /* ≈ 4.00 s @ LSI 32 kHz */
    /* 注意：F4 的 IWDG_InitTypeDef 只有 Prescaler / Reload 两个字段，
       没有 Window —— 窗口模式是 F7/L4 之后才集成进 IWDG 的。
       这里写 hiwdg.Init.Window 会报 "struct has no field Window"。
       F4 想用窗口功能得上 WWDG，本项目不需要。 */

    if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
    {
        printf("[IWDG  ] Init FAILED\r\n");
        return;
    }

    /* 调试时冻结 IWDG：内核被调试器停下时计数器也停，不会误复位 */
    __HAL_DBGMCU_FREEZE_IWDG();

    printf("[IWDG  ] started, timeout ~4000 ms (LSI)\r\n");
}

/* 喂狗。空闲任务每轮调一次，comm.c 的长阻塞路径（擦 Flash / 切 Bank）
   在进去之前也手动调一次。 */
void IWDG_Feed(void)
{
    HAL_IWDG_Refresh(&hiwdg);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

#if APP_WITH_BOOTLOADER
  SCB->VTOR = FLASH_BASE | 0x00010000u;

  __enable_irq();
#endif

  BOOT_TRACE('1');      /* 1 = 进 main 了（向量表 / MSP / scatter loading 都正常） */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* HAL 库初始化：配置 NVIC 优先级分组、SysTick（但 SysTick 会被 FreeRTOS 接管） */
  HAL_Init();
  BOOT_TRACE('2');      /* 2 = HAL_Init 过了（时基 TIM2 已配） */

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* 系统时钟配置：HSE 8MHz → PLL → 168MHz
     这是整个系统的心脏，时钟配错所有外设都会异常 */
  SystemClock_Config();
  BOOT_TRACE('3');      /* 3 = HSE 起振 + PLL 锁定成功 */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  MX_GPIO_Init();       // 1. GPIO 引脚（LED/按键/蜂鸣器/LCD CS/Flash CS）
  BOOT_TRACE('4');      /* 4 = GPIO 初始化过了 */
  MX_DMA_Init();        // 2. DMA 控制器（必须在使用 DMA 的外设之前）
  MX_I2C1_Init();       // 3. I2C1（PB8/PB9，接 24C02 + QMA6100P）
  MX_SPI2_Init();       // 4. SPI2（PB13/PB15，接 ILI9341 LCD）
  MX_TIM3_Init();       // 5. TIM3 PWM（PA6，舵机 50Hz）
  MX_USART1_UART_Init();// 6. USART1（PA9/PA10，调试串口 115200）
  BOOT_TRACE('5');      /* 5 = 调试串口重配完成，后面交给 printf */
  MX_USART3_UART_Init();// 7. USART3（PB10/PB11，ESP8266 AT 通信 115200）

  ESP8266_BootCapture(ESP_CAPTURE_BOOTLOG_MS);

  MX_SPI1_Init();       // 8. SPI1（PB3/PB4/PB5，W25Q128 Flash）
  /* USER CODE BEGIN 2 */

  /* 延时初始化（软件 I2C 和测试需要 delay_us/delay_ms） */
  delay_init(168);

  /* 点亮 LED0 表示程序已启动 */
  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);

  HAL_Delay(1000);
  printf("\r\n\r\n>>>>>> MiniWeather 启动 <<<<<<\r\n");
  LCD_Init();
 

  /*
     传感器 + 执行器初始化
     ⚠️ 必须在 MX_I2C1_Init() / MX_TIM3_Init() 之后（外设时钟已开）
     串口会打印每个器件的初始化结果，接线有问题一眼就能看出来
      */
  {
      uint8_t ret, bmp_id = 0;

      ret = SHTC3_Init();
      printf("[SHTC3 ] Init %s\r\n", (ret == 0) ? "OK" : "FAIL(1=复位失败 2=唤醒失败)");

      ret = BMP280_Init();
      if (ret == 0)
      {
          BMP280_ReadID(&bmp_id);
          printf("[BMP280] Init OK, id=0x%02X -> %s\r\n",
                 bmp_id, BMP280_IsBME280() ? "BME280" : "BMP280");
      }
      else
      {
          printf("[BMP280] Init FAIL, ret=%u (1=ID不对 2=通信失败 3=校准读失败)\r\n",
                 (unsigned int)ret);
      }

      ret = SGP30_Init();
      printf("[SGP30 ] Init %s (预热 15 秒内读数固定 400/0，属正常)\r\n",
             (ret == 0) ? "OK" : "FAIL(通信失败)");

      Servo_Init();        /* 舵机：启动 50Hz PWM，舵机置停止/中位 */
      BEEP_Init();         /* 蜂鸣器：确保上电不响 */
      RELAY_Init();        /* 继电器：默认断开 */
      printf("[ACTUAT] servo / beep / relay ready\r\n");

      KEY_Init();          /* 按键（引脚由 CubeMX 配好，这里只清状态） */
      LED_Init();          /* LED：上电先灭，由程序决定什么时候亮 */
      W25Q128_Init();      /* 板载 SPI Flash：片选拉高 + 唤醒 */
      {
          uint32_t fid = W25Q128_ReadID();
          printf("[W25Q128] id = 0x%06lX -> %s (%s)\r\n", (unsigned long)fid,
                 W25Q128_IDName(fid), W25Q128_IDIsValid(fid) ? "OK" : "BAD");
      }
  }

  {
      uint8_t ret = ESP8266_Init();
      const char *txt = (ret == 0) ? "OK, passthrough" :
                        (ret == 1) ? "no module (check P2 jumper)" :
                        (ret == 2) ? "WiFi join fail (check SSID/PWD)" :
                        (ret == 5) ? "CONFIG NOT SET -> edit WIFI_SSID/WIFI_PWD in esp8266.h" :
                                     "TCP connect fail (check server)";
      printf("[ESP8266] %s\r\n", txt);
  }

#if ESP_PROOF_LINK
  /*
     链路自证：逐条发 AT，把每条收到的【原始响应字节】全打出来。
     ⚠️ 必须放在 Comm_Init()/ESP8266_HandOver() 【之前】 —— 那之后 USART3
        就归协议栈的 DMA 循环接收了，AT 驱动不能再轮询读，会直接返回 4。
      */
  ESP8266_ProofLink();
#endif

  Comm_Init();
  ESP8266_HandOver();     /* ★ 告诉 AT 驱动：USART3 归协议栈了，别再轮询读它 */
  printf("[COMM  ] protocol ready, USART3 DMA RX @115200\r\n");

  /*
     硬件自检（9 项）
     结果同时打到串口(USART1) 和 LCD。会动作执行器（继电器咔哒、
     蜂鸣器短响一声、舵机转一下），这是正常的，不是故障。
     ⚠️ 必须在 RTOS 启动前跑：里面用了 delay_ms 和擦 Flash
      */
  {
      uint8_t total = 0;
      uint8_t pass  = APP_SelfTest(&total);
      printf("[SELFTEST] %u/%u passed\r\n", (unsigned int)pass, (unsigned int)total);
  }

  /* 自检结束，LED1 常亮表示"外设自检通过，准备进 RTOS" */
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);

  /* USER CODE END 2 */

  MX_IWDG_Init();

  /* 注意：FreeRTOS 启动后软件 I2C 会被硬件 I2C 覆盖
     正式项目中传感器用硬件 I2C（HAL_I2C_Master_Transmit 等） */

  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* 启动 FreeRTOS 调度器，之后控制权交给 RTOS，main 函数不会再返回 */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /* 电源时钟使能 + 电压调节器配置
     F407 在 168MHz 必须用 Scale1 模式（最高电压），否则不稳定 */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM2 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
/* TIM2 周期中断回调：每 1ms 触发一次 HAL_IncTick()
   因为 FreeRTOS 接管了 SysTick，所以 HAL 库的时基改用 TIM2
   这个函数让 HAL_Delay() / HAL_GetTick() 能正常工作 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM2)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
