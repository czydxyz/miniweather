#ifndef __BOOT_H__
#define __BOOT_H__

#include <stdint.h>


/* 上电主流程。正常情况不会返回（直接跳 App）；
   返回说明 App 区不可用，调用方应死循环等待救援。 */
void Boot_Run(void);

/* 极简串口日志（PA9/PA10，115200，轮询发送，不依赖 printf 重定向） */
void Boot_LogInit(void);
void Boot_Log(const char *fmt, ...);

/* 粗延时（毫秒）。为什么不用定时器：Bootloader 不初始化任何外设，
   SysTick 也没配，只能用循环空转。误差 ±10% 不影响任何逻辑，
   它唯一的用途就是【上电后等串口助手打开】和跳转前等最后一帧发完。 */
void Boot_DelayMs(uint32_t ms);

#endif /* __BOOT_H__ */
