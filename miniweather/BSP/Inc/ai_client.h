#ifndef __AI_CLIENT_H__
#define __AI_CLIENT_H__


#include <stdint.h>
#include "comm.h"

/* LCD 只有 ASCII 字模（asc2_1608[95][16]），且一行放不下太多字符，
   所以回传建议被强制收敛到 120 字符以内（与 PC 侧 MAX_ADVICE_LEN 一致） */
#define AI_ADVICE_MAX       121u     /* 120 个可见字符 + 1 个 '\0' */

/* 上报周期建议值（秒）。60 秒一次：大模型调用有费用/耗时，
   而微气候本身是慢变量，1 分钟粒度足够。 */
#define AI_REPORT_PERIOD_S  60u

/* ---- 初始化 ---- */

/* 打开 USART1 的接收中断（RXNE）+ 配 NVIC。
   ★ 必须在 MX_USART1_UART_Init() 之后调用。
     用中断而不用轮询：115200 下两个字节间隔只有 ~87us，
     而 TaskComm 是 5ms 周期，轮询必然丢字节（USART 没有 FIFO）。 */
void    AI_UartInit(void);

/* ---- 下行：收 PC 侧回传的建议文本 ---- */

/* 由 USART1 接收中断喂入：每收到一个字节调一次。 */
void    AI_CollectText(uint8_t b);

/* 成段喂入（一次多个字节）：由 Comm_Poll() 在 USART3/TCP 下行上调用。
   ★ 走 TCP 通道时，PC 回的建议是从 DMA 缓冲里【成段】取出来的，
     用这个函数比逐字节调 AI_CollectText() 更直接。 */
void    AI_FeedBytes(const uint8_t *buf, uint16_t n);

/* 取走一条完整建议（以 '\n' 结尾算一行）。
   返回 1 = 拿到一条（内容在 out 里，已保证纯 ASCII + 限长 + 带 '\0'）
        0 = 目前还没有完整的一行
   ★ 取完一定要把 out 送 LCD，本函数内部缓冲是"取走即清"。 */
uint8_t AI_TakeAdvice(char *out, uint16_t cap);

/* ---- 上行：报采样数据 ---- */

uint8_t AI_Report(const SensorSnapshot_t *snap);

/* ---- 降级：PC 侧不可达时的本地规则引擎 ---- */
/* 与 PC 侧 rule_engine() 逻辑一一对应（同一套阈值），保证"有没有网/有没有 PC"
   给出的结论方向一致，只是 PC 侧那份是大模型润色过的自然语言。 */
void    AI_LocalAdvice(const SensorSnapshot_t *snap, char *out, uint16_t cap);

#endif /* __AI_CLIENT_H__ */
