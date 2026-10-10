/* 舵机驱动（TIM3_CH1 = PA6，50Hz，PSC=83 / ARR=19999，1 计数 = 1us）
 * [约束] MX_TIM3_Init() 只做配置，必须再调 HAL_TIM_PWM_Start()，否则 PA6 恒为 0V。
 */
#include "servo.h"


/* 当前脉宽（us），调试和状态查询用 */
static uint16_t s_pulse = 1500;

#if (SERVO_TYPE == SERVO_TYPE_360)
/* 360 模式的状态：当前速度 + 定时转动剩余 tick 数 */
static int16_t  s_speed      = 0;
static uint16_t s_run_ticks  = 0;
#else
/* 180 模式的状态：当前角度（软件记录，舵机没有角度反馈） */
static float    s_angle      = 90.0f;
#endif

/**
 * @brief  写一个脉宽到 CCR1（内部函数）
 * @param  pulse_us 脉宽，单位微秒（1000~2000）
 * @note   __HAL_TIM_SET_COMPARE 展开就是 TIM3->CCR1 = pulse;
 */
static void Servo_WritePulse(uint16_t pulse_us)
{
    /* 限幅：CCR 不能超过 ARR，否则比较永远匹配不上 → 输出恒高 → 舵机一直堵转 */
    if (pulse_us > SERVO_PWM_PERIOD_US) pulse_us = SERVO_PWM_PERIOD_US;

    s_pulse = pulse_us;
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, pulse_us);
}

/* @brief  舵机初始化 */
void Servo_Init(void)
{
    /* ① 使能 CH1 输出（CC1E=1）+ 启动计数器（CEN=1） */
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);

    /* ② 舵机置"不动"状态，防止上电瞬间收到随机脉宽乱转 */
#if (SERVO_TYPE == SERVO_TYPE_360)
    s_speed     = 0;
    s_run_ticks = 0;
    Servo_WritePulse(SERVO_PULSE_STOP_US);
#else
    s_angle = 90.0f;
    Servo_WritePulse(SERVO_PULSE_MID_US);
#endif
}

#if (SERVO_TYPE == SERVO_TYPE_360)
/*
   360° 连续旋转舵机：脉宽 = 转速 + 方向
    */

/**
 * @brief  设置转速和方向（立即生效，一直转到再次调用为止）
 * @param  speed  -100 ~ +100
 *                 >0 正转（数值越大越快）
 *                 <0 反转
 *                 =0 停止
 * @note   内部会自动跳过"死区"，所以 speed=±1 时舵机也会慢慢转，
 *         而不是卡在死区里不动。
 */
void Servo_SetSpeed(int16_t speed)
{
    uint16_t pulse;

    if (speed > 100)  speed = 100;
    if (speed < -100) speed = -100;

    if (speed == 0)
    {
        pulse = SERVO_PULSE_STOP_US;                    /* 停止 */
    }
    else
    {
        /* 可用行程 = 500(中位到端点) - 死区，让 speed=±1 刚好踏出死区 */
        uint16_t range = 500 - SERVO_360_DEADBAND_US;

        if (speed > 0)                                  /* 正转 */
            pulse = SERVO_PULSE_STOP_US + SERVO_360_DEADBAND_US
                    + (uint16_t)((uint32_t)range * speed / 100);
        else                                            /* 反转 */
            pulse = SERVO_PULSE_STOP_US - SERVO_360_DEADBAND_US
                    - (uint16_t)((uint32_t)range * (-speed) / 100);
    }

    s_speed = speed;
    Servo_WritePulse(pulse);
}

/**
 * @brief  按给定速度转一段时间后自动停止（非阻塞）
 * @param  speed  -100~100
 * @param  ms     转动时长（毫秒）
 * @note   调用后立即返回，靠 Servo_Tick() 倒计时。
 *         时间到了自动 Servo_SetSpeed(0)。
 */
void Servo_Run(int16_t speed, uint16_t ms)
{
    if (speed == 0 || ms == 0)
    {
        Servo_SetSpeed(0);
        s_run_ticks = 0;
        return;
    }

    s_run_ticks = ms / SERVO_TICK_MS;     /* 换算成 tick 数 */
    if (s_run_ticks == 0) s_run_ticks = 1;

    Servo_SetSpeed(speed);
}

/**
 * @brief  每 SERVO_TICK_MS(10ms) 调用一次，用于定时转动的倒计时
 * @note   放在 TaskCtrl 的循环里：
 *            Servo_Tick();
 *            osDelay(10);
 *         这个函数的执行时间 <1us，不阻塞任何东西。
 */
void Servo_Tick(void)
{
    if (s_run_ticks == 0) return;         /* 没有定时任务 */

    s_run_ticks--;
    if (s_run_ticks == 0)
    {
        Servo_SetSpeed(0);                /* 时间到，停转 */
    }
}

/** @brief  是否正在定时转动（1=转着，0=停着） */
uint8_t Servo_IsBusy(void)
{
    return (s_run_ticks > 0) ? 1 : 0;
}

/** @brief  读当前转速（-100~100） */
int16_t Servo_GetSpeed(void)
{
    return s_speed;
}

/* ---- 语义层 ---- */
void Servo_Open(void)  { Servo_Run(SERVO_OPEN_SPEED,  SERVO_OPEN_RUN_MS);  }
void Servo_Close(void) { Servo_Run(SERVO_CLOSE_SPEED, SERVO_CLOSE_RUN_MS); }

#else
/*
   180° 角度舵机：脉宽 = 目标角度
    */

/**
 * @brief  转到指定角度
 * @param  angle  0.0 ~ 180.0（超出自动限幅）
 * @note   舵机是闭环器件：给脉宽它自己会转到位并保持住，不需要你"等它转完"。
 */
void Servo_SetAngle(float angle)
{
    uint16_t pulse;

    if (angle < 0.0f)   angle = 0.0f;
    if (angle > 180.0f) angle = 180.0f;

    /* 角度 → 脉宽。+0.5f 是为了四舍五入（uint16_t 强转是截断的） */
    pulse = (uint16_t)(SERVO_PULSE_MIN_US +
                       (SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) * angle / 180.0f + 0.5f);

    s_angle = angle;
    Servo_WritePulse(pulse);
}

/** @brief  读当前角度（软件记录值，不是从舵机读回来的） */
float Servo_GetAngle(void)
{
    return s_angle;
}

/* ---- 语义层 ---- */
void Servo_Open(void)  { Servo_SetAngle(SERVO_ANGLE_OPEN);  }
void Servo_Close(void) { Servo_SetAngle(SERVO_ANGLE_CLOSE); }
#endif /* SERVO_TYPE */

/**
 * @brief  读当前 CCR 值（调试用）
 * @note   没有示波器时，用万用表直流档量 PA6 对地电压：
 *         平均电压 = 3.3V × 脉宽/20000
 *         90°(1.5ms) ≈ 0.248V，0°(1.0ms) ≈ 0.165V，180°(2.0ms) ≈ 0.330V
 */
uint16_t Servo_GetPulse(void)
{
    return s_pulse;
}

/**
 * @brief  停止
 * @note   360 舵机：停转（但仍有力矩，能顶住外力）
 *         180 舵机：关闭 PWM 输出，舵机松力省电（一掰就动）
 */
void Servo_Stop(void)
{
#if (SERVO_TYPE == SERVO_TYPE_360)
    s_run_ticks = 0;
    Servo_SetSpeed(0);
#else
    HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_1);
#endif
}
