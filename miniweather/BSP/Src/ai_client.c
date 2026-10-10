/* AI 顾问：板端 JSON 上行 -> PC 调大模型 -> 建议文本下行 -> LCD 显示
 * [约束] USART1_IRQHandler 定义在本文件，绕开 HAL_UART_IRQHandler 直读寄存器。
 * [约束] AI_Report() 会往 USART3 写入 JSON，OTA 期间必须停止上报，否则与固件块抢缓冲。
 */

#include "ai_client.h"
#include "usart.h"          /* huart1（串口直连通道） */
#include "esp8266.h"        /* ESP8266_IsPassthrough / ESP8266_SendData（TCP 通道） */
#include <string.h>
#include <stdio.h>

/* 1) 下行文本行缓冲 */

/* 为什么不用环形缓冲而用"线性缓冲 + ready 标志"：
   建议是低频事件（几十秒一条）、数据量小（≤120 字节），
   线性缓冲代码简单、不会绕，出错概率低。高频数据才需要环形缓冲。 */
static char     s_line[AI_ADVICE_MAX];
static uint16_t s_len;
static uint8_t  s_ready;        /* 1 = 已收到完整一行，等上层取走 */

/* 把文本变成"板子能显示"的形式：
   ① 只留 ASCII 可见字符（LCD 没有中文字模，收到中文会显示成乱码块）
   ② 去掉 \r\n 等控制字符（LCD 不认换行）
   ③ 超长截断（屏上一行放不下） */
static void AI_Sanitize(char *s)
{
    uint16_t i, j;
    for (i = 0, j = 0; s[i] != '\0' && j < AI_ADVICE_MAX - 1; i++) {
        char c = s[i];
        if (c >= 0x20 && c <= 0x7E) {          /* 可见 ASCII */
            s[j++] = c;
        } else if (c == '\r' || c == '\n') {
            break;                              /* 行结束符：到此为止 */
        }
        /* 其余（中文首字节 / 控制字符）直接丢掉 */
    }
    s[j] = '\0';
}

void AI_CollectText(uint8_t b)
{
    /* 上一条还没取走 → 新的直接丢。
       ★ 这个"满了就不收"是故意的：宁可丢后来的字节，
         也不能让缓冲溢出或被半句话覆盖。 */
    if (s_ready) return;

    if (b == '\n') {                    /* 以 \n 作为一行的结束标志 */
        s_line[s_len] = '\0';
        AI_Sanitize(s_line);
        s_ready = 1;
        return;
    }
    if (b == '\r') return;              /* \r 直接忽略 */

    if (s_len < AI_ADVICE_MAX - 1) {
        s_line[s_len++] = (char)b;
        s_line[s_len]   = '\0';
    }
    /* 超长：静默丢弃多出来的字节，等 '\n' 来收尾 */
}

void AI_FeedBytes(const uint8_t *buf, uint16_t n)
{
    uint16_t i;
    if (buf == NULL) return;
    for (i = 0; i < n; i++) {
        AI_CollectText(buf[i]);
    }
}

uint8_t AI_TakeAdvice(char *out, uint16_t cap)
{
    if (!s_ready) return 0;                      /* 还没有完整一行 */

    if (out && cap) {
        /* strncpy 不保证结尾 '\0'（源串正好等于 cap 时会截断），所以手工补一刀 */
        strncpy(out, s_line, cap - 1);
        out[cap - 1] = '\0';
    }
    s_ready = 0;                                 /* 取走即清，准备收下一条 */
    s_len   = 0;
    s_line[0] = '\0';
    return 1;
}

/* 2) USART1 接收中断 */

void AI_UartInit(void)
{
    /* ① 先把可能残留的 ORE（溢出）清掉：F4 的规矩是"先读 SR 再读 DR" */
    if (USART1->SR & USART_SR_ORE) {
        volatile uint32_t tmp;
        tmp = USART1->SR; (void)tmp;
        tmp = USART1->DR; (void)tmp;
    }

    /* ② 只开 RXNE（接收中断）。
       ★ 不开 TXE：printf 走的是轮询发送，两套机制混用会打架。 */
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);

    HAL_NVIC_SetPriority(USART1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

void USART1_IRQHandler(void)
{
    uint32_t sr = USART1->SR;

    if (sr & USART_SR_RXNE) {
        uint8_t b = (uint8_t)(USART1->DR & 0x00FFu);   /* 读 DR 自动清 RXNE */
        AI_CollectText(b);
        return;
    }

    /* 溢出：字节来了但上一字节还没读走。按 F4 手册"读 SR + 读 DR"清标志，
       不要把整个串口关掉。 */
    if (sr & USART_SR_ORE) {
        volatile uint32_t tmp;
        tmp = USART1->SR; (void)tmp;
        tmp = USART1->DR; (void)tmp;
    }
}

/* 3) 上行：组 JSON 发出去 */

static uint16_t PutFixed1(char *p, int v)       /* v = 实际值 ×10 */
{
    uint16_t n = 0;
    int      ip, fp;

    if (v < 0) { p[n++] = '-'; v = -v; }
    ip = v / 10;
    fp = v % 10;
    n += (uint16_t)sprintf(&p[n], "%d.%d", ip, fp);
    return n;
}

uint8_t AI_Report(const SensorSnapshot_t *snap)
{
    char     buf[128];
    uint16_t n = 0;

    if (snap == NULL || snap->tick == 0) return 2;   /* 还没采到过数据 */

    /* ① 组帧。键名与 PC 侧 ai_advisor.py 里 d.get("temp") 等严格对应。
          两边任何一个拼错，PC 侧就会拿到 d.get(key, 0) 的默认值 ——
          所以 PC 端看到 0 值时先怀疑键名，而不是传感器坏了。 */
    n += (uint16_t)sprintf(&buf[n], "{\"temp\":");
    n += PutFixed1(&buf[n], snap->temp_01);
    n += (uint16_t)sprintf(&buf[n], ",\"humi\":");
    n += PutFixed1(&buf[n], (int)snap->humi_01);
    n += (uint16_t)sprintf(&buf[n], ",\"press\":%lu",
                           (unsigned long)snap->press_pa);
    n += (uint16_t)sprintf(&buf[n], ",\"co2eq\":%u",
                           (unsigned int)snap->co2_ppm);
    n += (uint16_t)sprintf(&buf[n], ",\"tvoc\":%u}",
                           (unsigned int)snap->tvoc_ppb);

    /* ② 补 \r\n：PC 侧按"行"切分（serial.readline），没有行结束符它会一直等 */
    if (((uint32_t)n + 2u) < (uint32_t)sizeof(buf)) {
        buf[n++] = '\r';
        buf[n++] = '\n';
        buf[n]   = '\0';
    } else {
        buf[sizeof(buf) - 1] = '\0';
        n = (uint16_t)(sizeof(buf) - 1);
    }

    if (ESP8266_IsPassthrough()) {
        if (ESP8266_SendData((const uint8_t *)buf, n) != 0) return 3;
        return 0;
    }
    if (HAL_UART_Transmit(&huart1, (uint8_t *)buf, n, 100) != HAL_OK) return 3;
    return 0;
}

/* 4) 降级：本地规则引擎 */

void AI_LocalAdvice(const SensorSnapshot_t *snap, char *out, uint16_t cap)
{
    int t10 = snap->temp_01;                 /* 温度 ×10 */
    int h10 = (int)snap->humi_01;            /* 湿度 ×10 */

    if (!out || !cap || !snap) return;

    /* 阈值与 PC 侧 rule_engine() 保持一致：
       CO2eq > 1000ppm → 开窗；TVOC > 300ppb → 通风；
       >28℃ 且 >70%RH → 又热又潮；<18℃ → 偏冷；<30%RH → 干燥。 */
    if (snap->co2_ppm > 1000) {
        sprintf(out, "Open windows now: CO2eq %u ppm is high.",
                (unsigned int)snap->co2_ppm);
    } else if (snap->tvoc_ppb > 300) {
        sprintf(out, "Ventilate room: TVOC %u ppb exceeds limit.",
                (unsigned int)snap->tvoc_ppb);
    } else if (t10 > 280 && h10 > 700) {
        sprintf(out, "Hot and humid: start fan or dehumidifier.");
    } else if (t10 < 180) {
        sprintf(out, "Room is cold (%d.%d C): consider heating.",
                t10 / 10, (t10 < 0 ? -t10 : t10) % 10);
    } else if (h10 < 300) {
        sprintf(out, "Air is dry (%d%%): use a humidifier.", h10 / 10);
    } else {
        sprintf(out, "Comfortable: temp %d.%d C, humidity %d%%.",
                t10 / 10, (t10 < 0 ? -t10 : t10) % 10, h10 / 10);
    }
    out[cap - 1] = '\0';        /* sprintf 超长时兜底截断 */
}
