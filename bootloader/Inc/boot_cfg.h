/* [约束] BOOT_LOG_DELAY_MS 联调完成后改 0（该延时每次上电都要等）。
 * [约束] 上电按住 KEY0(PE4，按下为低) 可强制重装一次。
 */
#ifndef __BOOT_CFG_H__
#define __BOOT_CFG_H__


#include "flash_layout.h"

/* 以下放 Bootloader 独有的配置（不跟 App 共享的东西） */

#define BOOT_LOG_DELAY_MS       1000u

/* 启动日志串口：USART1 / PA9 / PA10 / 115200。
   为什么不用 printf：那要开微库、写 fputc、还得防半主机。
   boot.c 里自己实现了一个轮询发送 + vsnprintf，十几行，完全可控。 */
#define BOOT_LOG_USART          USART1
#define BOOT_LOG_BAUDRATE       115200u

#define BOOT_FORCE_KEY_MS       800u

/* 强制重装键的端口引脚：KEY0 = PE4，按下为低（另一端接地，靠内部上拉） */
#define BOOT_FORCE_KEY_PORT     GPIOE
#define BOOT_FORCE_KEY_PIN      4u

#endif /* __BOOT_CFG_H__ */
