#ifndef __SERVO_H
#define __SERVO_H

#include "main.h"
#include "tim.h"


#define SERVO_TYPE_180      0
#define SERVO_TYPE_360      1

#define SERVO_TYPE          SERVO_TYPE_360    /* ← 改成 SERVO_TYPE_180 就是角度舵机 */

/* ================== 硬件参数（PSC=83 / ARR=19999 → 1 个计数 = 1us） ================== */
#define SERVO_PWM_PERIOD_US 20000     /* PWM 周期 20ms（50Hz），= ARR+1，改 CCR 时用来限幅 */

#if (SERVO_TYPE == SERVO_TYPE_360)

/* ---------- 360° 连续旋转舵机参数 ---------- */
/* 脉宽含义：1000us=全速反转  1500us=停止  2000us=全速正转 */
#define SERVO_PULSE_STOP_US 1500      /* 停止（中位） */
#define SERVO_PULSE_CW_US   2000      /* 全速正转（CCW/CW 方向以实物为准，反了就把这两个值互换） */
#define SERVO_PULSE_CCW_US  1000      /* 全速反转 */

/* 死区：360 舵机在 1500us 附近有一段"给了也不转"的区间，
   通常 ±(30~80)us。调速时直接跳过这段，否则小速度时舵机不动。
   实测方法：慢慢加大脉宽，找到"刚开始转"的那个值，填到这里。 */
#define SERVO_360_DEADBAND_US   60

/* 开窗 / 关窗：正(反)转多少毫秒。
   360 舵机没有位置反馈，只能靠时间控制 —— 若发现越跑越偏，
   ① 缩短时间 ② 每次上电先 Servo_Close() 到底做"归位" ③ 加限位开关 */
#define SERVO_OPEN_RUN_MS       1200    /* 开窗 = 正转 1.2 秒 */
#define SERVO_CLOSE_RUN_MS      1200    /* 关窗 = 反转 1.2 秒 */
#define SERVO_OPEN_SPEED        60      /* 开窗转速 0~100（太猛就调小） */
#define SERVO_CLOSE_SPEED      -60      /* 关窗转速 -100~0 */

/* Servo_Tick() 的调用周期（毫秒），必须和实际调用周期一致 */
#define SERVO_TICK_MS           10

#else

/* ---------- 180° 角度舵机参数 ---------- */
/* 1000~2000us 是 SG90 的安全线性区；0.5~2.5ms 是信号电气范围，
   但多数舵机在两端会顶到机械限位导致堵转发热，所以默认不用。 */
#define SERVO_PULSE_MIN_US  1000      /* 0°   */
#define SERVO_PULSE_MAX_US  2000      /* 180° */
#define SERVO_PULSE_MID_US  1500      /* 90°  （中位） */

#define SERVO_ANGLE_CLOSE   0         /* 关窗角度 */
#define SERVO_ANGLE_OPEN    90        /* 开窗角度 */

#endif /* SERVO_TYPE */

/* ================== 函数声明 ================== */
void     Servo_Init(void);                  /* 初始化：启动 PWM，舵机置停止/中位 */
void     Servo_Stop(void);                  /* 停止：360=停转；180=停 PWM 输出（松力省电） */

void     Servo_Open(void);                  /* 开窗（两种舵机通用语义） */
void     Servo_Close(void);                 /* 关窗（两种舵机通用语义） */

#if (SERVO_TYPE == SERVO_TYPE_360)
void     Servo_SetSpeed(int16_t speed);     /* -100~100，负=反转，0=停止（立即生效） */
void     Servo_Run(int16_t speed, uint16_t ms); /* 按给定速度转 ms 毫秒（非阻塞，需配合 Servo_Tick） */
void     Servo_Tick(void);                  /* 每 SERVO_TICK_MS 毫秒调用一次（360 模式必需） */
uint8_t  Servo_IsBusy(void);                /* 是否正在转动（1=转着） */
int16_t  Servo_GetSpeed(void);              /* 读当前速度 */
#else
void     Servo_SetAngle(float angle);       /* 转到 0~180°（自动限幅，执行时间 <1us） */
float    Servo_GetAngle(void);              /* 读当前角度（软件记录值，非实测） */
#endif

uint16_t Servo_GetPulse(void);              /* 读当前 CCR 值（调试用） */

#endif /* __SERVO_H */
