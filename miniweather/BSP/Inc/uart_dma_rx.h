#ifndef __UART_DMA_RX_H__
#define __UART_DMA_RX_H__

#include "main.h"
#include "usart.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 环形缓冲大小。115200 波特率下 1 字节 = 86.8us，
   256 字节绕一圈 = 22.2ms，所以调用周期必须 << 22ms（TaskComm 取 5ms） */
#ifndef URX_BUF_SIZE
#define URX_BUF_SIZE            256u
#endif

/* NDTR = "还剩多少字节没搬"，所以 写位置 = 总长 - NDTR。
   用 huart3.hdmarx 而不是 hdma_usart3_rx：前者是 HAL 在
   HAL_UART_MspInit 里用 __HAL_LINKDMA 绑好的，永远是有效的那个 */
#define URX_NDTR()              __HAL_DMA_GET_COUNTER(huart3.hdmarx)

typedef struct {
    uint8_t  buf[URX_BUF_SIZE];   /* ★ DMA 写的内存。CPU 只读 */
    uint16_t last;                /* 上次取走的位置（软件游标） */
    uint32_t overflow;            /* 检测到"被覆盖"的次数 */
    uint32_t taken_bytes;         /* 累计取走字节数 */
    uint32_t wrap;                /* DMA 绕圈次数（由 UartRx_NotifyDmaWrap 累加） */
    uint32_t last_abs;            /* 上次取走时的绝对写位置 = wrap*SIZE + cur */
    uint8_t  wrap_seen;           /* 1 = 已经在 DMA 中断里接过 UartRx_NotifyDmaWrap */
} UartRx_t;

/* 启动 DMA 循环接收（上电调一次，之后永不停） */
void     UartRx_Start(UartRx_t *r);

/* 取出自上次调用以来的新数据。
   out: 输出缓冲；cap: out 容量。返回实际取出字节数。
   内部自动处理环形回绕，调用方看到的就是一段"连续的线性数据"。 */
uint16_t UartRx_Take(UartRx_t *r, uint8_t *out, uint16_t cap);

/* 检测是否被 DMA 追尾覆盖 */
uint8_t  UartRx_CheckOverflow(UartRx_t *r);

/* 注册当前正在使用的实例（Comm_Init 里调一次）。
   注册之后下面的两个 ISR 钩子才知道要操作哪个缓冲。 */
void     UartRx_Register(UartRx_t *r);

/* ★ 在 DMA1_Stream1_IRQHandler 里、检测到"传输完成(TC)"时调一次。
   有了它才能精确判定"整圈被覆盖"，否则只能靠"任务周期足够快"来避免。
   不接也能正常工作（wrap_seen=0 时自动退化为不检测）。 */
void     UartRx_OnDmaTcFromISR(void);

/* 串口出错（ORE 溢出等）时调用，内部清标志 + 重启 DMA。
   不调的话 ORE 之后串口会彻底收不到数据，只能复位救场。 */
void     UartRx_OnUartError(void);

/* ORE（溢出错误）后 HAL 会停掉 RX，必须从错误回调里重启。
   在 HAL_UART_ErrorCallback 里调这个（内部会清标志 + 重启 DMA） */
void     UartRx_Recover(UartRx_t *r);

/* 读取当前 DMA 写位置（调试用） */
uint16_t UartRx_GetWritePos(UartRx_t *r);

#ifdef __cplusplus
}
#endif

#endif /* __UART_DMA_RX_H__ */
