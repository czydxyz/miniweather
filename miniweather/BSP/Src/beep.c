/* 蜂鸣器驱动（PF8，有源，音调固定）
 * [约束] BEEP_Alarm() 为阻塞式，FreeRTOS 任务中禁止调用（会卡住任务并触发看门狗）。
 */
#include "beep.h"
#include "delay.h"


/* 报警状态机 */
typedef struct
{
    uint8_t  running;       /* 1=正在报警 */
    uint8_t  level;         /* 当前蜂鸣器状态：1=响 0=停 */
    uint16_t times_left;    /* 还剩几声没响 */
    uint16_t tick;          /* 当前状态已经过了几个 tick */
    uint16_t on_ticks;      /* 响多少 tick */
    uint16_t off_ticks;     /* 停多少 tick */
} BEEP_State_t;

static BEEP_State_t s_beep = {0, 0, 0, 0, 0, 0};

/**
 * @brief  初始化蜂鸣器
 * @note   GPIO 由 CubeMX 配好，这里只保证上电是"关"的状态
 */
void BEEP_Init(void)
{
    BEEP_OFF();
    s_beep.running = 0;
    s_beep.level   = 0;
    s_beep.tick    = 0;
}

/**
 * @brief  阻塞式报警（裸机/测试用）
 * @warning FreeRTOS 任务里【禁止调用】，会卡住整个任务
 */
void BEEP_Alarm(uint16_t times, uint16_t on_ms, uint16_t off_ms)
{
    uint16_t i;

    for (i = 0; i < times; i++)
    {
        BEEP_ON();
        delay_ms(on_ms);
        BEEP_OFF();
        delay_ms(off_ms);
    }
}

/**
 * @brief  启动一次报警（非阻塞，调用后立即返回）
 * @param  times   响几声
 * @param  on_ms   每声响多久
 * @param  off_ms  每声之间停多久
 * @note   正在报警时再调用会被忽略（防重入）。
 *         想强制重新报警，先 BEEP_AlarmStop()。
 */
void BEEP_AlarmStart(uint16_t times, uint16_t on_ms, uint16_t off_ms)
{
    if (s_beep.running) return;           /* 正在响，忽略新请求 */
    if (times == 0)     return;

    /* 毫秒 → tick 数（至少要 1 个 tick，否则 0 会被当成"不用等"） */
    s_beep.on_ticks   = on_ms  / BEEP_TICK_MS;
    s_beep.off_ticks  = off_ms / BEEP_TICK_MS;
    if (s_beep.on_ticks  == 0) s_beep.on_ticks  = 1;
    if (s_beep.off_ticks == 0) s_beep.off_ticks = 1;

    s_beep.times_left = times;
    s_beep.tick       = 0;
    s_beep.level      = 1;
    s_beep.running    = 1;

    BEEP_ON();                            /* 第一声立刻开始 */
}

/**
 * @brief  报警状态机推进，每 BEEP_TICK_MS(10ms) 调用一次
 * @note   放在某个周期任务里：
 *            void StartTaskCtrl(void const *argument)
 *            {
 *                for(;;)
 *                {
 *                    BEEP_AlarmTick();
 *                    osDelay(10);
 *                }
 *            }
 */
void BEEP_AlarmTick(void)
{
    if (!s_beep.running) return;          /* 没在报警，直接返回 */

    s_beep.tick++;

    if (s_beep.level)                     /* ---- 当前处于"响" ---- */
    {
        if (s_beep.tick >= s_beep.on_ticks)
        {
            BEEP_OFF();                   /* 响够了，停 */
            s_beep.level = 0;
            s_beep.tick  = 0;
            s_beep.times_left--;

            if (s_beep.times_left == 0)   /* 全部响完 */
            {
                s_beep.running = 0;
            }
        }
    }
    else                                  /* ---- 当前处于"停" ---- */
    {
        if (s_beep.tick >= s_beep.off_ticks)
        {
            BEEP_ON();                    /* 停够了，开始下一声 */
            s_beep.level = 1;
            s_beep.tick  = 0;
        }
    }
}

/** @brief  是否正在报警（1=响着） */
uint8_t BEEP_IsAlarming(void)
{
    return s_beep.running;
}

/** @brief  立即停止报警 */
void BEEP_AlarmStop(void)
{
    BEEP_OFF();
    s_beep.running = 0;
    s_beep.level   = 0;
    s_beep.tick    = 0;
}

/**
 * @brief  "嘀"一声（非阻塞）
 * @param  ms  响多久
 */
void BEEP_Beep(uint16_t ms)
{
    BEEP_AlarmStop();         /* 先停掉可能正在进行的报警 */
    BEEP_AlarmStart(1, ms, 0);
}
