/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines.
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
#include "main.h"
#include "stm32f4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "uart_dma_rx.h"     /* USART3 DMA 环形缓冲：TC 计数 / ORE 恢复 */
#include "cmsis_os.h"        /* osSystickHandler()：FreeRTOS 的心跳入口 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* ========== 中断服务函数文件（ISR）==========
   这里是所有中断的入口函数，CubeMX 自动生成，一般不需要手改
   重点关注下面三个：

   DMA1_Stream1_IRQHandler → USART3 的 DMA 接收完成/半传输中断
     作用：DMA 搬运完数据后通知 HAL 库，HAL 再调用回调函数
     优先级 6（在 FreeRTOS 安全区，可以调 FromISR API）

   TIM2_IRQHandler → HAL 时基定时器（替代 SysTick）
     作用：每 1ms 触发一次 HAL_IncTick()，让 HAL_Delay 正常工作
     优先级 15（最低，不抢任何外设中断）

   USART3_IRQHandler → ESP8266 串口中断
     作用：处理空闲中断（收到一帧 AT 响应），通知任务处理
     优先级 6（和 DMA 一样，在 FreeRTOS 安全区）

   ETH_IRQHandler → 以太网中断
     优先级 5（刚好在边界，lwIP 用它接收网络数据包）
*/

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern ETH_HandleTypeDef heth;
extern DMA_HandleTypeDef hdma_usart3_rx;
extern UART_HandleTypeDef huart3;
extern TIM_HandleTypeDef htim2;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f4xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles DMA1 stream1 global interrupt.
  */
/* DMA1_Stream1 中断：USART3 DMA 接收
   DMA 搬运完数据后进这里 → HAL_DMA_IRQHandler 内部判断是传输完成还是半传输
   → 再调用 HAL_UART_RxCpltCallback 等回调函数 */
void DMA1_Stream1_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Stream1_IRQn 0 */
  /* 循环模式下 DMA 每绕完一圈（256 字节）就会触发一次 TC 中断。
     先判 TC 标志并通知环形缓冲"又绕了一圈" —— 有了这个计数，
     UartRx_Take 才能区分"绕了整整一圈"和"一个字节都没收到"，
     进而精确判定数据是否被覆盖（overflow）。 */
  if (__HAL_DMA_GET_FLAG(&hdma_usart3_rx, DMA_FLAG_TCIF1_5) != RESET)
  {
    __HAL_DMA_CLEAR_FLAG(&hdma_usart3_rx, DMA_FLAG_TCIF1_5);
    UartRx_OnDmaTcFromISR();
  }
  /* USER CODE END DMA1_Stream1_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart3_rx);
  /* USER CODE BEGIN DMA1_Stream1_IRQn 1 */

  /* USER CODE END DMA1_Stream1_IRQn 1 */
}

/**
  * @brief This function handles TIM2 global interrupt.
  */
/* TIM2 中断：HAL 库时基（替代 SysTick）
   每 1ms 触发一次 → HAL_IncTick() 让 uwTick +1 → HAL_Delay 依赖此计数 */
void TIM2_IRQHandler(void)
{
  /* USER CODE BEGIN TIM2_IRQn 0 */

  /* USER CODE END TIM2_IRQn 0 */
  HAL_TIM_IRQHandler(&htim2);
  /* USER CODE BEGIN TIM2_IRQn 1 */

  /* USER CODE END TIM2_IRQn 1 */
}

/**
  * @brief This function handles USART3 global interrupt.
  */
/* USART3 中断：ESP8266 AT 通信
   主要处理空闲中断（IDLE）→ 收到一帧完整 AT 响应后通知任务
   后面会在这里加 HAL_UARTEx_RxEventCallback 回调处理 */
void USART3_IRQHandler(void)
{
  /* USER CODE BEGIN USART3_IRQn 0 */

  /* USER CODE END USART3_IRQn 0 */
  HAL_UART_IRQHandler(&huart3);
  /* USER CODE BEGIN USART3_IRQn 1 */

  /* USER CODE END USART3_IRQn 1 */
}

/**
  * @brief This function handles Ethernet global interrupt.
  */
/* 以太网中断：YT8512C PHY 收到网络数据包时触发
   优先级 5（刚好在 FreeRTOS 临界线上，不能调更低优先级的 FromISR） */
void ETH_IRQHandler(void)
{
  /* USER CODE BEGIN ETH_IRQn 0 */

  /* USER CODE END ETH_IRQn 0 */
  HAL_ETH_IRQHandler(&heth);
  /* USER CODE BEGIN ETH_IRQn 1 */

  /* USER CODE END ETH_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* ==========================================================================
 * 串口错误回调（ORE 溢出 / 帧错误 / 奇偶错 / 噪声）
 *
 * ★ 必须处理：ORE（OverRun Error，收到新字节但上个字节还没被取走）发生后，
 *   HAL 会直接把 RX 关掉，表现是"串口突然彻底收不到数据"，只能复位救场。
 *   这里清标志 + 重启 DMA，让链路自己恢复。
 *   5ms 轮询下几乎不会触发，但一旦触发没有它就是死机。
 * ========================================================================*/
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3)
  {
    UartRx_OnUartError();
  }
}

/* ==========================================================================
 * ★★★ SysTick 中断：FreeRTOS 的心脏 ★★★
 *
 * 【结论】本文件里【不要】写 SysTick_Handler，它已经由 FreeRTOS 的 port.c 提供。
 *
 * 链路是这样的：
 *   ① Core/Inc/FreeRTOSConfig.h 里有
 *          #define xPortSysTickHandler SysTick_Handler
 *      这是一条【宏改名】，不是注释。
 *   ② portable/RVDS/ARM_CM4F/port.c 里定义的是
 *          void xPortSysTickHandler(void) { ... }
 *      因为 port.c 也 include 了 FreeRTOSConfig.h，预处理后它就被改写成
 *          void SysTick_Handler(void) { ... }
 *      于是链接时 SysTick 向量自然指向 RTOS 的节拍处理函数。
 *   → 如果在本文件再写一个 void SysTick_Handler(void)，
 *     两个同名强符号 → Keil 报 L6200E: Symbol SysTick_Handler multiply defined。
 *
 * 至于正点原子的 Core/Src/delay.c：那里确实也有一个 SysTick_Handler，
 * 但它被 #if SYS_SUPPORT_OS 包着，而 delay.h 里【没有】定义这个宏
 * （未定义 → 预处理按 0 处理）→ 整块被裁掉，不会参与链接。这是对的：
 * 那份实现调的是 uC/OS-II 的 OS_CPU_SysTickHandler()，本工程根本没用 uCOS。
 *
 * ★ 另一个必须知道的：cubeMX 已把 HAL 的时基改成 TIM2（SysTick 让给 RTOS），
 *   所以 HAL 的 1ms 计数走 stm32f4xx_hal_timebase_tim.c 里 TIM2 的
 *   PeriodElapsedCallback → HAL_IncTick()。
 *   SysTick 这条路上【没有任何 HAL_IncTick()】，也别去加：
 *   两边都 +1 会让 HAL_Delay() 快一倍。
 * ========================================================================*/


/* USER CODE END 1 */
