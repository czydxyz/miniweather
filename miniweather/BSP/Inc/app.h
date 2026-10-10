#ifndef __APP_H__
#define __APP_H__

#include "main.h"
#include <stdint.h>


/* ---------------- 自动控制阈值（想改灵敏度改这里） ---------------- */
#define CTRL_CO2_ALARM_PPM      1200u    /* CO₂ 当量超过这个值 → 报警 + 开窗 + 开风扇 */
#define CTRL_CO2_RECOVER_PPM    900u     /* 低于这个值 → 撤销报警（★留了回差，防止反复触发） */
#define CTRL_TEMP_ALARM_01      300      /* 温度 ×10 超过 30.0℃ → 开风扇 */
#define CTRL_TEMP_RECOVER_01    270      /* 低于 27.0℃ → 关风扇 */
#define CTRL_HUMI_ALARM_01      700      /* 湿度 ×10 超过 70%RH → 报警提示（结露风险） */

/* 报警重复间隔（秒）：避免每 500ms 响一次，吵得没法调试 */
#define CTRL_ALARM_INTERVAL_S   60u

/* ---------------- 自检用的临时地址（都选在不影响业务的角落） ---------------- */
#define TEST_FLASH_ADDR         0x080E0000u   /* 内部 Flash S11（128KB 空闲区）的开头 */
#define TEST_W25Q_ADDR          0x00FFF000u   /* W25Q128 最后一个 4KB 扇区 */
#define TEST_EEPROM_ADDR        0xF8u         /* AT24C02 末 8 字节（避开 SGP30 基线区） */

/* ---------------- API ---------------- */

/* 上电自检：逐项测试所有外设，结果打到调试串口(USART1) + LCD。
   ⚠️ 阻塞执行（要等传感器、擦 Flash），必须在 FreeRTOS 启动【之前】调用。
   返回：通过的项数；总项数通过指针 total 传出 */
uint8_t APP_SelfTest(uint8_t *total);

/* 屏幕：把传感器数据 / 设备状态 / 运行时间画到 LCD 上（每 500ms 调一次） */
void    APP_Display(void);

/* 按键交互：KEY0=开窗  KEY1=关窗  KEY2=蜂鸣器  KEY_UP=风扇翻转（每 500ms 调一次） */
void    APP_KeyPoll(void);

/* 自动控制：按阈值决定开窗 / 风扇 / 报警（每 500ms 调一次） */
void    APP_CtrlTick(void);

/* 当前设备状态（供显示函数和协议层查询） */
typedef struct {
    uint8_t  window_open;    /* 1 = 窗已开 */
    uint8_t  fan_on;         /* 1 = 风扇开 */
    uint8_t  alarming;       /* 1 = 正在报警 */
    uint8_t  manual;         /* 1 = 处于手动模式（按过键后 30 秒内不自动干预） */
    uint32_t manual_until_s; /* 手动模式截止的运行秒数 */
} AppState_t;

AppState_t *APP_GetState(void);

/* 运行秒数（由 TaskSensor 每 1 秒调用一次推进） */
void    APP_SetUptime(uint32_t sec);
uint32_t APP_GetUptime(void);

/* SGP30 基线存/取（由 TaskSensor 每 1 秒调用一次，内部自己算时间）
   规则（见第 7 节）：无基线要跑满 12 小时才可信；之后每 1 小时存一次；
   断电 7 天内有效 → 存在 AT24C02 的 0x10~0x17。 */
void    APP_BaselineTick(uint32_t sec);

#endif /* __APP_H__ */
