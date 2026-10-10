/* Bootloader 入口
 * [约束] 不配置时钟：跑在复位默认 HSI 16MHz（SystemInit 不会调 SetSysClock()）。
 * [约束] Keil 工程 Options -> C/C++ -> Define 必须有 STM32F407xx。
 */
#include "stm32f4xx.h"
#include "boot_cfg.h"      /* BOOT_LOG_DELAY_MS 在这里 */
#include "boot.h"

int main(void)
{
    /* 调试串口：上电就能在 PC 串口助手看到搬运过程（115200 8N1） */
    Boot_LogInit();

    /* 上电先等一会儿，给 PC 上的串口助手留出打开 COM 口的时间。
       否则日志在人切回窗口之前就发完了，看着像"什么都没打印"。
       联调完把 boot_cfg.h 里的 BOOT_LOG_DELAY_MS 改成 0。 */
    Boot_DelayMs(BOOT_LOG_DELAY_MS);

    /* 主流程：读标志 → 按需搬运 → 跳 App
       正常情况不会返回（已经跳走了）；
       返回说明运行区不可用，只能停在这里等人来救。 */
    Boot_Run();

    /* App 区坏了：停在这儿。
       不要死循环里狂闪灯或者复位重启 —— 那会把"我需要救援"这个信号淹掉。
       串口已经打印了 HALT 原因，接上串口就知道该怎么办（重新烧 App）。 */
    while (1) {
        __WFI();        /* 省电等待，别空转发热 */
    }
}
