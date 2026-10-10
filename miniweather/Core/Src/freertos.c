/* 任务栈与优先级：defaultTask(Normal,128) / TaskSensor(High,512) /
 * TaskDisplay(AboveNormal,512) / TaskComm(Normal,1024)
 * [约束] 栈大小单位是「字」不是字节；configTOTAL_HEAP_SIZE = 15360。
 * [约束] osThreadCreate() 返回值必须检查：heap 不足返回 NULL，不报错、直接进 HardFault。
 * [约束] 喂狗在 vApplicationIdleHook()：该函数禁止阻塞、禁止 printf、禁止等信号量。
 * [约束] 传感器数据结构必须清零初始化，否则会用栈垃圾当真值。
 * [约束] TaskComm 周期 5ms；n==0 时也要调用 Proto_Tick。
 * [约束] 禁止在 stm32f4xx_it.c 手写 SysTick_Handler / SVC / PendSV（FreeRTOSConfig.h 已宏改名）。
 */
/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "comm.h"           /* 自定义协议落地层：Comm_Poll / Comm_UpdateSensor */
#include "ai_client.h"      /* AI 顾问：AI_Report() 上行采样 JSON */
#include "servo.h"          /* 舵机非阻塞状态机：Servo_Tick */
#include "beep.h"           /* 蜂鸣器非阻塞状态机：BEEP_AlarmTick */
#include "app.h"            /* 应用层：显示 / 按键 / 自动控制 / 基线管理 */
#include "esp8266.h"        /* Wi-Fi 状态维护：ESP8266_Process */
#include "shtc3.h"
#include "bmp280.h"
#include "sgp30.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

#define CHECK_TASK(h, name)  do {                                             \
        if ((h) == NULL) {                                                    \
            printf("[FATAL] 任务 %s 创建失败：FreeRTOS heap 不足\r\n", (name));\
            Error_Handler();                                                  \
        }                                                                     \
    } while (0)

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* ========== FreeRTOS 任务句柄 ==========
   每个任务都有一个句柄，用于后续控制（挂起、恢复、删除等）
*/
osThreadId defaultTaskHandle;     // 默认任务：初始化 lwIP，然后空转
osThreadId tasksensorHandle;      // 传感器采集任务：读 SHTC3/BMP280/SGP30，最高优先级
osThreadId TaskdisplayHandle;     // 显示任务：刷新 LCD 屏幕
osThreadId TaskcommHandle;        // 通信任务：ESP8266 AT + MQTT + LLM，栈最大(1024)
osMessageQId QueuesesorHandle;    // 传感器数据队列：采集任务→显示/通信任务

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void const * argument);
void StartTasksensor(void const * argument);
void StartTaskdisplay(void const * argument);
void StartTaskcomm(void const * argument);

extern void MX_LWIP_Init(void);
void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* GetIdleTaskMemory prototype (linked to static allocation support) */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize );

/* Hook prototypes */
void vApplicationIdleHook(void);

/* USER CODE BEGIN 2 */
/* Idle Hook（空闲任务钩子）
   当所有任务都在等待时，FreeRTOS 调用这个函数
   注意：这个函数绝不能阻塞（不能 osDelay、不能 xQueueReceive）
   用途：后面在这里喂看门狗（HAL_IWDG_Refresh），因为只有系统没死锁才会进入空闲 */
__weak void vApplicationIdleHook( void )
{

   IWDG_Feed();
}
/* USER CODE END 2 */

/* USER CODE BEGIN GET_IDLE_TASK_MEMORY */
static StaticTask_t xIdleTaskTCBBuffer;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize )
{
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCBBuffer;
  *ppxIdleTaskStackBuffer = &xIdleStack[0];
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
  /* place for user code */
}
/* USER CODE END GET_IDLE_TASK_MEMORY */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

#ifndef USE_SENSOR_QUEUE
#define USE_SENSOR_QUEUE        0
#endif
#if USE_SENSOR_QUEUE
  osMessageQDef(Queuesesor, 4, uint16_t);
  QueuesesorHandle = osMessageCreate(osMessageQ(Queuesesor), NULL);
#endif

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* definition and creation of defaultTask */
  osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 128);
  defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);
  CHECK_TASK(defaultTaskHandle, "defaultTask");

  /* definition and creation of tasksensor */
  osThreadDef(tasksensor, StartTasksensor, osPriorityHigh, 0, 512);
  tasksensorHandle = osThreadCreate(osThread(tasksensor), NULL);
  CHECK_TASK(tasksensorHandle, "tasksensor");

  /* definition and creation of Taskdisplay */
  osThreadDef(Taskdisplay, StartTaskdisplay, osPriorityAboveNormal, 0, 512);
  TaskdisplayHandle = osThreadCreate(osThread(Taskdisplay), NULL);
  CHECK_TASK(TaskdisplayHandle, "Taskdisplay");

  /* definition and creation of Taskcomm */
  osThreadDef(Taskcomm, StartTaskcomm, osPriorityNormal, 0, 1024);
  TaskcommHandle = osThreadCreate(osThread(Taskcomm), NULL);
  CHECK_TASK(TaskcommHandle, "Taskcomm");

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
/* defaultTask：初始化 lwIP（以太网协议栈），然后空转
   lwIP 必须在 RTOS 任务里初始化，因为它依赖 osDelay/信号量等 RTOS API
   初始化完成后这个任务就没什么事了，osDelay(1) 让出 CPU */
void StartDefaultTask(void const * argument)
{
  /* init code for LWIP */
  MX_LWIP_Init();
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* USER CODE BEGIN Header_StartTasksensor */
/**
* @brief Function implementing the tasksensor thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTasksensor */
void StartTasksensor(void const * argument)
{
  /* USER CODE BEGIN StartTasksensor */
  /* ⚠️ 周期 1000ms：SGP30 手册 6.3 要求每 1 秒发一次 0x2008，
       否则内部动态基线算法退化（不报错，只是读数越来越飘）。 */
  uint32_t        sec = 0;
  SHTC3_Data_t    th  = {0};
  BMP280_Data_t   bp  = {0};
  SGP30_Data_t    air = {0};
  SensorSnapshot_t snap;
  uint8_t         th_valid = 0;   /* SHTC3 至少成功读过一次才允许做湿度补偿 */
  uint8_t         ai_once  = 0;   /* AI 顾问"上电首发"标记：0=还没发过第一次 */

  memset(&snap, 0, sizeof(snap));

  for(;;)
  {
    /* ① 维护运行秒数（心跳命令和显示都要用） */
    Comm_SetUptime(sec);
    APP_SetUptime(sec);

    /* ② 读温湿度。失败时保留上一次的值并标记 —— 不要用 0 覆盖，
          否则屏幕会闪一下 0.0℃，比"数据没更新"更难看 */
    if (SHTC3_Read(&th) == 0) {
        snap.temp_01 = (int16_t)(th.temperature * 10.0f);
        snap.humi_01 = (uint16_t)(th.humidity   * 10.0f);
        th_valid = 1;                       /* 拿到过真数据，才允许往下做补偿 */
    } else {
        printf("[SHTC3 ] read fail\r\n");
    }

    /* ③ 读气压 */
    if (BMP280_Read(&bp) == 0) {
        snap.press_pa = (uint32_t)bp.pressure;
    } else {
        printf("[BMP280] read fail\r\n");
    }

    /* ④ 读 CO₂ 当量 / TVOC */
    if (SGP30_Read(&air) == 0) {
        snap.co2_ppm  = air.co2;
        snap.tvoc_ppb = air.tvoc;

        /* 湿度补偿：MOx 传感器对水汽敏感，不补偿 CO₂ 读数会偏高。
           每 10 秒写一次就够（温度变化没那么快），省 I2C 带宽 */
        /* ★ 判据必须是 th_valid（"真的读到过"），不能是 snap.humi_01 != 0：
           后者在 SHTC3 一直失败时可能仍是上一轮残留的非零值，
           而 th 里装的其实是未初始化的栈数据。 */
        if ((sec % 10u) == 0u && th_valid) {
            SGP30_SetAbsoluteHumidity(th.humidity, th.temperature);
        }
    } else {
        printf("[SGP30 ] read fail\r\n");
    }

    /* ⑤ 基线存/取（上电恢复 + 每小时保存） */
    APP_BaselineTick(sec);

    /* ⑥ 交给协议层。Comm_UpdateSensor 内部进临界区整块拷贝，
          协议任务读到的永远是一份完整快照，不会被读到"半新半旧" */
    if (snap.tick == 0xFFFFFFFFu) snap.tick = 1;
    else                          snap.tick++;
    Comm_UpdateSensor(&snap);

    {
        /* C90 要求声明在块首，这里正好是块首 */
        OtaState st   = Comm_GetOtaState();
        uint8_t  busy = (st == OTA_ERASING) || (st == OTA_READY) || (st == OTA_RECEIVING);

        if (!busy)
        {
            if (ai_once == 0u)
            {
                if ((sec >= 1u) && (ESP8266_IsPassthrough() || (sec >= 30u)))
                {
                    AI_Report(&snap);
                    ai_once = 1u;       /* 只发这一次，之后交给下面的周期逻辑 */
                }
            }
            else if ((sec != 0u) && ((sec % AI_REPORT_PERIOD_S) == 0u))
            {
                AI_Report(&snap);
            }
        }
    }

    sec++;
    osDelay(1000);
  }
  /* USER CODE END StartTasksensor */
}

/* USER CODE BEGIN Header_StartTaskdisplay */
/**
* @brief Function implementing the Taskdisplay thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskdisplay */
void StartTaskdisplay(void const * argument)
{
  /* USER CODE BEGIN StartTaskdisplay */
  /* 这个任务实际上承担了三件事（都只需要 500ms 级刷新，合并省一个栈）：
       ① 刷 LCD
       ② 扫按键（消抖 10ms，500ms 扫一次足够跟手）
       ③ 跑自动控制决策（阈值判断 + 执行器动作）          */
  for(;;)
  {
    APP_Display();
    APP_KeyPoll();
    APP_CtrlTick();

    osDelay(500);
  }
  /* USER CODE END StartTaskdisplay */
}

/* USER CODE BEGIN Header_StartTaskcomm */
/**
* @brief Function implementing the Taskcomm thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskcomm */
void StartTaskcomm(void const * argument)
{
  /* USER CODE BEGIN StartTaskcomm */
  uint32_t cnt  = 0;    /* 10ms 节拍计数（蜂鸣器 / 舵机状态机） */
  uint32_t cnt2 = 0;    /* 5 秒节拍计数（Wi-Fi 状态维护） */
  for(;;)
  {
    Comm_Poll();                    /* 取字节 → 解帧 → 处理命令 → 回应答 */

    if (++cnt >= 2) {               /* 5ms × 2 = 10ms */
      cnt = 0;
      BEEP_AlarmTick();
      Servo_Tick();
    }

    if (++cnt2 >= 1000) {           /* 5ms × 1000 = 5s */
      cnt2 = 0;
      ESP8266_Process();            /* 透传态下会直接返回，不影响数据流 */
    }

    osDelay(COMM_TASK_PERIOD_MS);
  }
  /* USER CODE END StartTaskcomm */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */
