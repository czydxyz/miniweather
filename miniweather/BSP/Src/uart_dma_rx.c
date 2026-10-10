/* USART3 DMA 循环接收环形缓冲
 * [约束] IDLE 中断默认关闭：协议自带长度字段与 CRC16，轮询 NDTR 已足够。
 * [约束] 必须检查启动返回值：ESP 初始化失败会打断 DMA 启动，是「能发不能收」的头号原因。
 */
#include "uart_dma_rx.h"
#include <string.h>
#include "usart.h"      /* huart3 */
#include <stdio.h>      /* printf：启动失败时必须喊出来，否则故障是静默的 */

void UartRx_Start(UartRx_t *r)
{
    memset(r->buf, 0, sizeof(r->buf));
    r->last       = 0;
    r->overflow   = 0;
    r->taken_bytes = 0;
    r->wrap       = 0;
    r->last_abs   = 0;
    r->wrap_seen  = 0;

    /* __HAL_UART_ENABLE_IT(&huart3, UART_IT_IDLE); */

    {
        HAL_StatusTypeDef st = HAL_UART_Receive_DMA(&huart3, r->buf, URX_BUF_SIZE);

        if (st != HAL_OK) {
            /* 先报出来，让人知道出过事（别让故障静默） */
            printf("[URX   ] !! DMA 启动失败 st=%d RxState=%d -> 复位状态后重试\r\n",
                   (int)st, (int)huart3.RxState);

            /* 和 ESP_TxBlink 里同一个坑的同一剂药：把状态机强行按回初始态，
               逼 HAL 重新走一遍完整配置（DMA 句柄重绑 + 重新使能） */
            huart3.gState  = HAL_UART_STATE_READY;
            huart3.RxState = HAL_UART_STATE_READY;
            st = HAL_UART_Receive_DMA(&huart3, r->buf, URX_BUF_SIZE);

            printf("[URX   ] retry st=%d\r\n", (int)st);
        } else {
            printf("[URX   ] DMA circular RX started, buf=%u byte(s)\r\n",
                   (unsigned int)URX_BUF_SIZE);
        }
    }
}

uint16_t UartRx_Take(UartRx_t *r, uint8_t *out, uint16_t cap)
{
    uint16_t cur;
    uint16_t n1, n2;
    uint16_t n = 0;

    /* ① 问 DMA："你写到哪了？"
          NDTR 是"还剩多少字节要搬"，减一下就是已经写了多少 */
    cur = (uint16_t)(URX_BUF_SIZE - (uint16_t)URX_NDTR());
    if (cur >= URX_BUF_SIZE) cur = 0;      /* 防御：NDTR=0 时正好写完一圈 */

    if (r->wrap_seen) {
        uint32_t abs = r->wrap * (uint32_t)URX_BUF_SIZE + cur;
        uint32_t delta = abs - r->last_abs;
        if (delta > URX_BUF_SIZE) r->overflow++;
        r->last_abs = abs;
    }

    /* ③ 没有新数据 */
    if (cur == r->last) return 0;

    if (cur > r->last) {
        /* ---- 情形 1：没有回绕，一段连续 ---- */
        n1 = (uint16_t)(cur - r->last);
        n2 = 0;
    } else {
        /* ---- 情形 2：已经绕回开头了，两段 ---- */
        n1 = (uint16_t)(URX_BUF_SIZE - r->last);
        n2 = cur;
    }

    /* ④ 拷贝，注意别把 out 撑爆 */
    if (n1 > cap) n1 = cap;
    if (n1 > 0) {
        memcpy(out, &r->buf[r->last], n1);
        n = n1;
    }

    if (n2 > 0 && n < cap) {
        if (n2 > (uint16_t)(cap - n)) n2 = (uint16_t)(cap - n);
        memcpy(&out[n], &r->buf[0], n2);
        n = (uint16_t)(n + n2);
    }

    /* ⑤ 推进软件游标。
          ★ 即使上面因为 cap 不够被截断了，也要把 last 推到 cur，
            否则下次会重复读到旧数据 —— 重复的数据插进帧中间会导致 CRC 错，
            比丢几个字节难查得多。原则：**宁可丢，不可重**。 */
    r->last = cur;
    r->taken_bytes += n;

    return n;
}

uint8_t UartRx_CheckOverflow(UartRx_t *r)
{
    return (r->overflow > 0) ? 1 : 0;
}

/*
 * 注册 + 中断钩子
 * 为什么要有 Register：中断回调是全局的、无参数的，它没法知道该操作哪个
 * 环形缓冲。所以上电时把实例指针记下来（Comm_Init 里调 UartRx_Register）。
  */
static UartRx_t *s_active = NULL;

void UartRx_Register(UartRx_t *r)
{
    s_active = r;
}

void UartRx_OnDmaTcFromISR(void)
{
    if (s_active == NULL) return;
    s_active->wrap++;
    s_active->wrap_seen = 1;
}

void UartRx_OnUartError(void)
{
    if (s_active == NULL) return;
    UartRx_Recover(s_active);
}

/* 在 DMA1_Stream1_IRQHandler 里、检测到传输完成(TC)标志时调用一次 */
void UartRx_NotifyDmaWrap(UartRx_t *r)
{
    r->wrap++;
    r->wrap_seen = 1;
}

/*
 * ORE 恢复：串口溢出错误后 HAL 会把 RX 关掉，表现为"彻底收不到数据"。
 * 必须在 HAL_UART_ErrorCallback 里清标志并重启 DMA，否则只能复位救场。
  */
void UartRx_Recover(UartRx_t *r)
{
    /* ① 清 ORE：F4 要求先读 SR 再读 DR（HAL 的宏就是这么干的） */
    __HAL_UART_CLEAR_PEFLAG(&huart3);

    /* ② 重启 DMA（这里不得不 Stop 一次，因为状态机已经乱了） */
    HAL_UART_DMAStop(&huart3);
    r->last = 0;
    r->wrap = 0;
    r->wrap_seen = 0;
    r->last_abs = 0;
    HAL_UART_Receive_DMA(&huart3, r->buf, URX_BUF_SIZE);
}

uint16_t UartRx_GetWritePos(UartRx_t *r)
{
    uint16_t cur = (uint16_t)(URX_BUF_SIZE - (uint16_t)URX_NDTR());
    if (cur >= URX_BUF_SIZE) cur = 0;
    (void)r;
    return cur;
}
