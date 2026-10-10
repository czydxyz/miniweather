/* ESP8266 AT 驱动（USART3 PB10/PB11，115200）
 * [约束] 初始化阶段独占 USART3（阻塞查询）；Comm_Init() 之后本驱动不得再碰 huart3。
 * [约束] 调用顺序必须是 ESP8266_Init() -> Comm_Init()，反了会冲突。
 * [约束] 进透传后 AT 不再被解析，退出需发 "+++"（前后各留 1s 静默）。
 * [约束] ESP8266_BootCapture() 必须在 main() 中尽早调用：模块上电约 300ms 就吐完 bootlog。
 * [约束] ESP_PROOF_LINK 必须为 0：透传态下的 AT 诊断会把数据发进 TCP。
 */
#include "esp8266.h"
#include "delay.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* 最近一次 AT 响应（调试时打印出来看非常有用） */
static char  s_resp[ESP8266_RX_BUF_SIZE];
/* 当前状态 */
static ESP8266_Status_t s_status = ESP_WIFI_DISCONNECTED;
/* 是否已进入透传（透传态下不能再发 AT） */
static uint8_t s_passthrough = 0;
/* USART3 是否已被协议栈接管（Comm_Init 开了 DMA 循环接收）
   ★ 置 1 之后本驱动彻底不碰 huart3：
     HAL_UART_Receive（轮询）和 HAL_UART_Receive_DMA 都要把 RxState 改成 BUSY_RX，
     两边同时用 → 后调用的拿 HAL_BUSY → 收不到任何字节 → 每条 AT 都超时。 */
static uint8_t s_uart_taken = 0;

static char     s_bootlog[192];
static uint16_t s_bootlen = 0;
static uint8_t  s_bootdone = 0;

static void ESP_BootPush(uint8_t c)
{
    if (s_bootlen < (uint16_t)(sizeof(s_bootlog) - 1)) {
        s_bootlog[s_bootlen++] = (char)c;
        s_bootlog[s_bootlen]   = '\0';
    }
}

/* 前向声明：下面的函数要互相调用，顺序上分开写更清楚 */
static void ESP_HWResetBody(void);
static void ESP_EscapeTransparent(void);

void ESP8266_HandOver(void)
{
    s_uart_taken = 1;
}

/* 底层发送 */
static void ESP_SendRaw(const uint8_t *data, uint16_t len)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)data, len, 1000);
}

static void ESP_SendStr(const char *str)
{
    ESP_SendRaw((const uint8_t *)str, (uint16_t)strlen(str));
}

/*
 * 拼命令：AT+CWJAP="ssid","pwd" 这种带参数的
 * 用 snprintf 而不是 sprintf —— 后者不检查长度，参数长了会写爆缓冲区
  */
static void ESP_Cmd(char *out, uint16_t outlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, outlen, fmt, ap);
    va_end(ap);
}

/*
 * 把串口上残存的旧数据清干净
 * 不清的话，上一条指令的 OK 会被下一条指令当成自己的响应，
 * 表现为"指令明明失败了却返回成功" —— 这类幽灵 bug 极难查
  */
static void ESP_FlushRx(uint32_t ms)
{
    uint8_t  c;
    uint32_t t0 = HAL_GetTick();

    while (HAL_GetTick() - t0 < ms) {
        if (HAL_UART_Receive(&huart3, &c, 1, 5) != HAL_OK) break;   /* 5ms 没数据就认为清完了 */
    }
}

/* 前向声明：下面的 SendCmd() 要调它，而函数体定义在「排障三件套」那一节。
   ⚠️ 少了这一行，ARMCC 会把调用处的 ESP_UartRecover 隐式声明成
      int ESP_UartRecover(...)，跟真正的 static void 定义打架 → error #159 */
static void ESP_UartRecover(void);

void ESP8266_BootCapture(uint32_t ms)
{
    uint32_t t0;

    if (s_bootdone || s_uart_taken) return;      /* 只抓第一次 */
    s_bootdone = 1;

    t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < ms) {
        if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_ORE)) {
            (void)huart3.Instance->SR;                       /* 先读 SR */
            ESP_BootPush((uint8_t)(huart3.Instance->DR & 0x00FFu));  /* 再读 DR（数据还在） */
            continue;
        }
        if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {
            ESP_BootPush((uint8_t)(huart3.Instance->DR & 0x00FFu));
        }
    }
}

/* 把字节收进「可读文本」缓冲：只保留可打印 ASCII + \r\n。
   前面那串 ROM 日志是 74880 波特、在 115200 下采出来全是乱码字节，
   混进 buffer 会淹没后面的 banner；更要命的是一旦混进一个 0x00，
   C 字符串就在这里被截断，strstr 永远找不到后面的 "ready"。 */
static void ESP_TextPush(char *buf, uint16_t *idx, uint16_t cap, uint8_t d)
{
    if (d == '\r' || d == '\n' || (d >= 0x20u && d <= 0x7Eu)) {
        if (((uint32_t)*idx + 1u) < (uint32_t)cap) {
            buf[(*idx)++] = (char)d;
            buf[*idx]     = '\0';
        }
    }
}

uint8_t ESP_WaitReady(char *text, uint16_t textlen, uint32_t maxms,
                      uint16_t *seen_bytes, uint32_t *elapsed)
{
    uint32_t t0;
    uint16_t i     = 0;
    uint16_t total = 0;

    if (text && textlen) text[0] = '\0';
    if (seen_bytes) *seen_bytes = 0;
    if (elapsed)    *elapsed    = 0;
    if (maxms == 0) maxms = ESP_CAPTURE_BOOTLOG_MS;

    t0 = HAL_GetTick();

    while ((uint32_t)(HAL_GetTick() - t0) < maxms) {
        uint8_t d = 0;
        uint8_t got = 0;

        /* ORE 优先：DR 里还躺着最后一个到达的字节，按"先读 SR 再读 DR"取走，
           顺便就把 ORE 标志清了，不会像 __HAL_UART_CLEAR_OREFLAG 那样把字节丢掉。 */
        if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_ORE)) {
            (void)huart3.Instance->SR;
            d   = (uint8_t)(huart3.Instance->DR & 0x00FFu);
            got = 1;
        } else if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {
            d   = (uint8_t)(huart3.Instance->DR & 0x00FFu);
            got = 1;
        }

        if (got) {
            total++;
            if (text) ESP_TextPush(text, &i, textlen, d);
            /* 收到 ready 就立刻收工，不必傻等满 maxms */
            if (text && strstr(text, "ready") != NULL) {
                if (seen_bytes) *seen_bytes = total;
                if (elapsed)    *elapsed    = (uint32_t)(HAL_GetTick() - t0);
                return 1;
            }
        }
    }

    if (seen_bytes) *seen_bytes = total;
    if (elapsed)    *elapsed    = (uint32_t)(HAL_GetTick() - t0);
    return 0;
}

/*
 * 硬件复位模块（PF6）
 * 探索者 ATK-MODULE 接口的 RST 接 PF6；正点原子官方例程的时序是
 * 拉低 100ms → 拉高 → 等 500ms 让模块启动完。
  */
static void ESP_HWResetBody(void)
{
    GPIO_InitTypeDef gpio;

    __HAL_RCC_GPIOF_CLK_ENABLE();
    gpio.Pin   = GPIO_PIN_6;
    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOF, &gpio);

    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_6, GPIO_PIN_RESET);   /* RST 低电平有效 */
    delay_ms(100);
    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_6, GPIO_PIN_SET);     /* 开漏写 1 = 释放（高阻 + 上拉） */
    delay_ms(500);                                         /* 等模块跑起来 */
}

void ESP8266_HWReset(void)
{
    ESP_HWResetBody();

    /* 复位之后日志要重新抓（旧的那份已经作废） */
    s_bootlen  = 0;
    s_bootdone = 0;
    s_bootlog[0] = '\0';
}

static void ESP_EscapeTransparent(void)
{
    delay_ms(1000);          /* 前置静默：期间不能有任何字节 */
    ESP_SendStr("+++");      /* ★ 不能带 \r\n！带了就被当成普通数据发给服务器 */
    delay_ms(1000);          /* 后置静默 */
    ESP_FlushRx(200);        /* 倒掉 "OK" 之类残留 */
}

uint8_t ESP8266_SendCmd(const char *cmd, const char *expect, uint32_t timeout)
{
    uint16_t i = 0;
    uint8_t  c;
    uint32_t t0;

    if (s_passthrough) return 1;          /* 透传态下 AT 无效 */
    if (s_uart_taken)  return 4;          /* ★ USART3 已是协议的 DMA，不能再轮询读 */

    ESP_UartRecover();                    /* ★ 清 ORE + 复位 HAL 状态机，否则可能永远收不到 */
    ESP_FlushRx(30);                      /* 先清掉上一条的残留 */
    memset(s_resp, 0, sizeof(s_resp));

    ESP_SendStr(cmd);
    ESP_SendStr("\r\n");

    while (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {
        uint8_t d = (uint8_t)(huart3.Instance->DR & 0x00FFu);
        if (i < (uint16_t)(sizeof(s_resp) - 1)) {
            s_resp[i++] = (char)d;
            s_resp[i]   = '\0';
        }
    }
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_ORE)) {
        __HAL_UART_CLEAR_OREFLAG(&huart3);
    }
    /* 抢救到的字节里可能已经有答案了，先判一次再进入等待循环 */
    if (i > 0 && expect == NULL) return 0;
    if (expect != NULL && strstr(s_resp, expect) != NULL) return 0;

    t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < timeout) {
        if (HAL_UART_Receive(&huart3, &c, 1, 5) == HAL_OK) {
            if (i < (uint16_t)(sizeof(s_resp) - 1)) {
                s_resp[i++] = (char)c;
                s_resp[i]   = '\0';
            }
            /* 命中期望串 → 成功 */
            if (expect == NULL) return 0;
            if (strstr(s_resp, expect) != NULL) return 0;
            /* 提前失败判定：别死等到超时 */
            if (strstr(s_resp, "ERROR") != NULL) return 2;
            if (strstr(s_resp, "FAIL")  != NULL) return 3;
        }
    }
    return 1;                             /* 超时 */
}

/* 基础指令 */
uint8_t ESP8266_Test(void)
{
    /* 连发两次：刚上电时模块可能还在打印启动信息，第一次常被冲掉 */
    if (ESP8266_SendCmd("AT", "OK", 500) == 0) return 0;
    return ESP8266_SendCmd("AT", "OK", ESP8266_CMD_TIMEOUT);
}


static void ESP_UartRecover(void)
{
    volatile uint8_t tmp;

    /* ① ORE：F4 的清除序列是「先读 SR 再读 DR」，宏内部已按此实现 */
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_ORE)) {
        __HAL_UART_CLEAR_OREFLAG(&huart3);
    }
    /* ② 读空数据寄存器，顺带清掉 RXNE */
    while (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {
        tmp = (uint8_t)(huart3.Instance->DR & 0x00FFu);
    }
    (void)tmp;
    /* ③ 把 HAL 状态机拉回 READY，否则后续 Receive 一律返回 BUSY/ERROR */
    huart3.ErrorCode = HAL_UART_ERROR_NONE;
    huart3.RxState   = HAL_UART_STATE_READY;
    huart3.gState    = HAL_UART_STATE_READY;
}

static void ESP_DumpUartCfg(void)
{
    uint32_t moder = GPIOB->MODER;
    uint32_t afrh  = GPIOB->AFR[1];          /* AFR[1] 管 Pin8~15 */
    uint8_t  m10   = (uint8_t)((moder >> 20) & 0x3u);   /* PB10 -> bit21:20 */
    uint8_t  m11   = (uint8_t)((moder >> 22) & 0x3u);   /* PB11 -> bit23:22 */
    uint8_t  a10   = (uint8_t)((afrh  >>  8) & 0xFu);   /* PB10 -> AFRH bit11:8 */
    uint8_t  a11   = (uint8_t)((afrh  >> 12) & 0xFu);   /* PB11 -> AFRH bit15:12 */

    /* 期望值：MODER=0b10(复用功能) AF=7(USART3) */
    printf("[ESP8266] CFG: PB10 MODER=%u AF=%u | PB11 MODER=%u AF=%u  (want 2 / 7)\r\n",
           (unsigned int)m10, (unsigned int)a10,
           (unsigned int)m11, (unsigned int)a11);
    printf("[ESP8266] CFG: USART3 CR1=0x%04X (UE,TE,RE) BRR=0x%04X PCLK1=%lu Hz\r\n",
           (unsigned int)(USART3->CR1 & 0xFFFFu),
           (unsigned int)(USART3->BRR & 0xFFFFu),
           (unsigned long)HAL_RCC_GetPCLK1Freq());
}

static uint16_t ESP_LoopbackProbe(char *out, uint16_t outlen)
{
    static const char probe[] = "AT\r\n";
    uint16_t n = 0;
    uint8_t  i;

    if (outlen == 0) return 0;
    out[0] = '\0';

    ESP_UartRecover();                       /* 清 ORE + 复位 HAL 状态机 */

    for (i = 0; i < (uint8_t)(sizeof(probe) - 1u); i++) {
        uint8_t  tx = (uint8_t)probe[i];
        uint32_t t0;

        HAL_UART_Transmit(&huart3, &tx, 1, 10);      /* 阻塞发 1 个字节 */

        /* 立刻等回显/回环回来的那一个字节。20ms 足够 115200 下 1 个字节来回
           （1 个字节约 87us），给这么宽是为了容忍杜邦线接触不良带来的抖动。 */
        t0 = HAL_GetTick();
        while (HAL_GetTick() - t0 < 20u) {
            if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {
                uint8_t d = (uint8_t)(huart3.Instance->DR & 0x00FFu);
                if (n < (uint16_t)(outlen - 1u)) {
                    out[n++] = (char)d;
                    out[n]   = '\0';
                }
                break;
            }
        }

        /* 万一还是溢出了，清掉标志，否则后面每次 HAL_UART_Receive 都直接失败
           —— 这个坑本文件 ESP_UartRecover 上面有完整记录。 */
        if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_ORE)) {
            __HAL_UART_CLEAR_OREFLAG(&huart3);
        }
    }
    return n;
}

uint16_t ESP8266_Listen(uint32_t ms, char *out, uint16_t outlen)
{
    uint16_t n = 0;
    uint8_t  c;
    uint32_t t0;

    if (s_uart_taken) return 0;              /* 串口已归协议栈，别碰 */
    if (out && outlen) out[0] = '\0';

    /* ① 先抢救：模块可能早就把话说完了，字节还躺在 DR 里没人读
       （实测抓到过一个 'r'，是 "ready" 的首字母）。
       ⚠️ 必须放在 Recover 之前——Recover 清 ORE 时会读一次 DR，把这一字节吃掉。 */
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {
        uint8_t d = (uint8_t)(huart3.Instance->DR & 0x00FFu);
        if (out && outlen) { out[0] = (char)d; out[1] = '\0'; }
        n = 1;
    }

    /* ② 再恢复：清 ORE + 复位 HAL 状态机，否则可能半个字都收不到 */
    ESP_UartRecover();

    t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < ms) {
        uint32_t sr = USART3->SR;

        if (sr & USART_SR_RXNE) {
            uint8_t d = (uint8_t)(USART3->DR & 0x00FFu);   /* 读 DR 清 RXNE */
            if (out && outlen && n < (uint16_t)(outlen - 1)) {
                out[n]   = (char)d;
                out[n+1] = '\0';
            }
            n++;
            continue;
        }
        /* 溢出：F4 的清法是「先读 SR 再读 DR」，上面已读 SR，这里读 DR 即可 */
        if (sr & USART_SR_ORE) {
            (void)(USART3->DR & 0x00FFu);
        }
    }
    (void)c;
    return n;
}

static void ESP_TxBlink(void)
{
    GPIO_InitTypeDef gpio;
    int i;

    printf("[ESP8266] TX blink on PB10 x20 (meter/LED check, ~12s)...\r\n");

    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Pin   = GPIO_PIN_10;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* 20 个周期 × (300ms 高 + 300ms 低) = 12 秒。
       为什么要这么久：要拿着表笔/LED 沿 PB10 → P2中排 → 飞线 → 模块 P1 第3脚
       逐点量过去，太快根本来不及换测点。直流电压档会看到 ≈1.65V 的平均值在跳；
       接 LED(串 1k) 则直接看到闪。 */
    for (i = 0; i < 20; i++) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_SET);
        delay_ms(300);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
        delay_ms(300);
    }

    huart3.gState    = HAL_UART_STATE_RESET;
    huart3.RxState   = HAL_UART_STATE_RESET;
    MX_USART3_UART_Init();
}

void ESP8266_DumpResp(const char *tag)
{
    uint16_t i, n = (uint16_t)strlen(s_resp);

    printf("[ESP8266] %s: %u byte(s)\r\n", (tag ? tag : "resp"), (unsigned int)n);
    if (n == 0) {
        printf("[ESP8266]   (nothing -> 没上电 / P2 跳线在 RS232 侧 / TX-RX 没交叉)\r\n");
        return;
    }

    printf("[ESP8266]   txt: ");
    for (i = 0; i < n && i < 96; i++) {
        char c = s_resp[i];
        printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
    }
    printf("\r\n[ESP8266]   hex: ");
    for (i = 0; i < n && i < 32; i++) printf("%02X ", (unsigned int)(unsigned char)s_resp[i]);
    printf("\r\n");
}

uint8_t ESP8266_AutoBaud(void)
{
    /* 常见波特率排在最前的先试。74880 是 ESP8266 boot ROM 的特有波特率，
       刷机模式/启动日志用它输出 —— 收到乱码时往往就是它。 */
    static const uint32_t bauds[] = {115200u, 9600u, 57600u, 38400u, 19200u, 74880u};
    uint8_t i;

    if (s_uart_taken) return 1;

    for (i = 0; i < (uint8_t)(sizeof(bauds) / sizeof(bauds[0])); i++) {
        /* 只改 BRR，不重建 GPIO/DMA：
           HAL_UART_Init 在 gState != RESET 时不会重调 MspInit，
           正好符合我们的需要。 */
        huart3.Init.BaudRate = bauds[i];
        if (HAL_UART_Init(&huart3) != HAL_OK) continue;

        if (ESP8266_SendCmd("AT", "OK", 400) == 0) {
            printf("[ESP8266] baud locked @ %lu\r\n", (unsigned long)bauds[i]);
            return 0;
        }
    }

    /* 全都没回：恢复默认 115200，别把串口留在奇怪的波特率上 */
    huart3.Init.BaudRate = 115200;
    HAL_UART_Init(&huart3);
    return 1;
}

uint8_t ESP8266_Reset(void)
{
    uint8_t ret = ESP8266_SendCmd("AT+RST", "OK", 3000);
    delay_ms(2000);                 /* 复位后模块要 1~2 秒才起来，期间不响应 */
    ESP_FlushRx(200);               /* 倒掉启动信息（末尾一般是 ready） */
    s_status = ESP_WIFI_DISCONNECTED;
    s_passthrough = 0;
    return ret;
}

uint8_t ESP8266_SetMode(uint8_t mode)
{
    char cmd[32];
    ESP_Cmd(cmd, sizeof(cmd), "AT+CWMODE=%u", (unsigned int)mode);
    return ESP8266_SendCmd(cmd, "OK", ESP8266_CMD_TIMEOUT);
}

uint8_t ESP8266_JoinAP(const char *ssid, const char *pwd)
{
    char cmd[128];

    ESP_Cmd(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, pwd);
    /* 连 WiFi 是最慢的一步：认证 + DHCP 可能要 10 秒以上 */
    if (ESP8266_SendCmd(cmd, "OK", 15000) == 0) {
        s_status = ESP_WIFI_CONNECTED;
        return 0;
    }
    ESP_Cmd(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, pwd);
    if (ESP8266_SendCmd(cmd, "OK", 15000) == 0) {     /* 重试一次很值得：路由器偶尔会拒绝第一次 */
        s_status = ESP_WIFI_CONNECTED;
        return 0;
    }
    return 1;
}

uint8_t ESP8266_CheckWifiStatus(void)
{
    /* AT+CWJAP? 返回 +CWJAP:"ssid","mac",ch,rssi */
    if (ESP8266_SendCmd("AT+CWJAP?", "+CWJAP", 2000) == 0) {
        s_status = ESP_WIFI_CONNECTED;
        return 0;
    }
    if (s_status == ESP_TCP_CONNECTED) s_status = ESP_TCP_DISCONNECTED;
    else                               s_status = ESP_WIFI_DISCONNECTED;
    return 1;
}

char *ESP8266_GetLocalIP(char ip[16])
{
    char *p;

    ip[0] = '\0';
    if (ESP8266_SendCmd("AT+CIFSR", "OK", 2000) != 0) return ip;

    /* 响应形如：+CIFSR:STAIP,"192.168.1.23" —— 把引号里的东西抠出来 */
    p = strstr(s_resp, "STAIP,\"");
    if (p == NULL) p = strstr(s_resp, "APIP,\"");
    if (p == NULL) return ip;

    p = strchr(p, '"');
    if (p == NULL) return ip;
    p++;

    {
        uint8_t i = 0;
        while (*p != '\0' && *p != '"' && i < 15) ip[i++] = *p++;
        ip[i] = '\0';
    }
    return ip;
}

/* TCP 连接与透传 */
uint8_t ESP8266_ConnectTCP(const char *ip, uint16_t port)
{
    char cmd[96];

    /* 先设成单连接，否则多连接模式下每条数据都要带 id，很麻烦 */
    ESP8266_SendCmd("AT+CIPMUX=0", "OK", 1000);

    ESP_Cmd(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u", ip, (unsigned int)port);
    if (ESP8266_SendCmd(cmd, "OK", 8000) == 0) {
        s_status = ESP_TCP_CONNECTED;
        return 0;
    }
    /* 连不上也可能是已经连着（返回 ALREADY CONNECTED），这种情况算成功 */
    if (strstr(s_resp, "ALREADY") != NULL) {
        s_status = ESP_TCP_CONNECTED;
        return 0;
    }
    s_status = ESP_TCP_DISCONNECTED;
    return 1;
}

uint8_t ESP8266_EnterPassthrough(void)
{
    /* ① 开透传模式 */
    if (ESP8266_SendCmd("AT+CIPMODE=1", "OK", 1000) != 0) return 1;
    /* ② 发 CIPSEND 进入透传，成功标志是 '>'（不是 OK！） */
    if (ESP8266_SendCmd("AT+CIPSEND", ">", 2000) != 0) return 2;

    s_passthrough = 1;
    s_status = ESP_TCP_CONNECTED;
    return 0;
}

uint8_t ESP8266_ExitPassthrough(void)
{
    uint8_t ret;

    if (!s_passthrough) return 0;

    /* ★ "+++" 三字符前后必须各有 1 秒的静默，否则模块把它当普通数据发出去 */
    delay_ms(1000);
    ESP_SendStr("+++");
    delay_ms(1000);

    s_passthrough = 0;
    ret = ESP8266_SendCmd("AT", "OK", 1000);   /* 验证确实退回指令态了 */
    return ret;
}

/* ★ 查询当前是否处于透传态（见 esp8266.h 里的说明）。
   实现只是一行 —— 但这一行是"通道选择"的唯一判据，暴露出来比各处猜要可靠得多。 */
uint8_t ESP8266_IsPassthrough(void)
{
    return s_passthrough ? 1u : 0u;
}

uint8_t ESP8266_SendData(const uint8_t *data, uint16_t len)
{
    if (!s_passthrough) {
        /* 普通模式：先发长度，等 '>' 再发数据 */
        char cmd[32];
        ESP_Cmd(cmd, sizeof(cmd), "AT+CIPSEND=%u", (unsigned int)len);
        if (ESP8266_SendCmd(cmd, ">", 2000) != 0) return 1;
    }
    ESP_SendRaw(data, len);
    return 0;
}

uint8_t ESP8266_CloseTCP(void)
{
    uint8_t ret;

    if (s_passthrough) ESP8266_ExitPassthrough();

    ret = ESP8266_SendCmd("AT+CIPCLOSE", "OK", 1000);
    s_status = ESP_TCP_DISCONNECTED;
    return ret;
}

/*
 * 完整初始化
 *   关回显 → AT 测试 → 设 STA → 连 WiFi →（可选）连 TCP + 进透传
 * 返回：0=全部成功 / 1=模块无响应 / 2=WiFi 连不上 / 3=TCP 连不上
  */
uint8_t ESP8266_Init(void)
{
    char     ip[16];
    char     boot[192];
    uint16_t n;
    uint8_t  at_ok = 0;
    uint8_t  ready_seen = 0;
    uint32_t ready_ms   = 0;

    s_status = ESP_WIFI_DISCONNECTED;
    s_passthrough = 0;
    s_uart_taken = 0;                         /* 允许重复调用本函数 */

#if ESP_USE_HW_RESET
    ESP8266_HWReset();                        /* 先把模块复位到干净状态（顺带清掉旧日志） */
#endif

    /* ① 拿开机日志：优先用 main() 里提前抓的那份（最完整，见 ESP8266_BootCapture）。
       万一下面平台没调过 ESP8266_BootCapture()，这里再补听一次。 */
    if (s_bootdone) {
        strncpy(boot, s_bootlog, sizeof(boot) - 1);
        boot[sizeof(boot) - 1] = '\0';
        n = s_bootlen;
    } else {
        /* ★ 一直听到模块打出 "ready" 为止（最多 ESP_WAIT_READY_MS 毫秒）。
           打印出来的毫秒数就是模块真实启动时间，以后不用再拿秒表量。 */
        ready_seen = ESP_WaitReady(boot, sizeof(boot), ESP_WAIT_READY_MS,
                                   &n, &ready_ms);
        printf("[ESP8266] ready-prompt: %s after %u ms (limit %u)\r\n",
               ready_seen ? "seen" : "NOT seen",
               (unsigned int)ready_ms, (unsigned int)ESP_WAIT_READY_MS);
    }

    printf("[ESP8266] bootlog: %u byte(s)\r\n", (unsigned int)n);
    if (n) {
        strncpy(s_resp, boot, sizeof(s_resp) - 1);   /* 借用 DumpResp 把原文打出来 */
        s_resp[sizeof(s_resp) - 1] = '\0';
        ESP8266_DumpResp("boot");

        /* ★ 日志里出现 WIFI / CONNECT，说明模块一开机就在自己联网或连服务器 ——
           这种情况下 AT 指令基本是被无视的（透传态下 AT 被当成数据）。
           十有八九是上一次设过 AT+SAVETRANSLINK（保存透传），下面走 "+++" 夺回控制权。 */
        if (strstr(s_resp, "CONNECT") != NULL || strstr(s_resp, "WIFI") != NULL) {
            printf("[ESP8266] hint: module auto-connecting at boot (SAVETRANSLINK?) -> will try +++\r\n");
        }
    } else {
        /* ★ bootlog 为 0 不再等于"一定硬件坏"：双固件下 Bootloader 的 1 秒日志延时
           + 跳转 + App 初始化，等到这里开始听时模块早就吐完了，属预期现象。
           真正的硬件判定交给下面 AutoBaud 之后的 DIAG 行。 */
        printf("[ESP8266]   (silence: wire/VCC/P2-jumper -- or bootlog already expired"
               " under bootloader build)\r\n");
    }

    /* ② 先按 115200 直接试一次 —— 正常情况这一步就通了，不额外花时间 */
    if (ESP8266_SendCmd("AT", "OK", 500) == 0) at_ok = 1;

    if (!at_ok) {
        printf("[ESP8266] no AT reply -> try +++ (1s silence each side)...\r\n");
        ESP_EscapeTransparent();
        if (ESP8266_SendCmd("AT", "OK", 800) == 0) {
            printf("[ESP8266] recovered: module was in passthrough\r\n");
            at_ok = 1;
        }
    }

    /* ④ 还不通 → 波特率自适应：115200 不行就依次试 9600/57600/38400/19200/74880。
       这一步能区分「硬件不通」和「波特率不对」。 */
    if (!at_ok && ESP8266_AutoBaud() != 0) {
        ESP8266_DumpResp("AT");               /* 把最后一次收到的原始字节打出来 */

        /* ★ 配置级证据：把引脚复用 / 收发使能 / BRR / PCLK1 直接打出来。
           这四行能一次性判定"是 MCU 自己没配好"还是"对面没人说话"。 */
        ESP_DumpUartCfg();

        {
            uint16_t at_len = (uint16_t)strlen(s_resp);

            if (n && at_len) {
                printf("[ESP8266] DIAG: BOTH directions have bytes -> module replies but "
                       "not 'OK' (wrong baud? not AT firmware?)\r\n");
            } else if (n) {
                printf("[ESP8266] DIAG: RX alive, TX dead -> P2 needs BOTH jumpers on GBC side\r\n");
            } else if (at_len) {
                printf("[ESP8266] DIAG: MCU TX alive (self-echo/loopback seen), "
                       "RX silent -> module not driving PB11 (no power / not seated / RST?)\r\n");
            } else {
                printf("[ESP8266] DIAG: RX+TX both silent -> check module VCC / P2 jumper / TX-RX cross\r\n");
            }
        }

        {
            uint16_t m = ESP8266_Listen(800, NULL, 0);
            printf("[ESP8266] DIAG: passive listen = %u byte(s) -> %s\r\n",
                   (unsigned int)m,
                   m ? "physical link ALIVE, it is a module-STATE problem"
                     : "no traffic at all, suspect wiring/VCC/jumper");
        }

#if ESP_LOOPBACK_PROBE
        {
            char     lb[16];
            uint16_t k = ESP_LoopbackProbe(lb, sizeof(lb));
            printf("[ESP8266] LOOPBACK probe: %u/4 byte(s) hex:", (unsigned int)k);
            {
                uint16_t q;
                for (q = 0; q < k; q++) printf(" %02X", (unsigned int)(unsigned char)lb[q]);
            }
            printf(" -> %s\r\n",
                   k ? "MCU PB10/PB11 + USART3 OK (external loopback detected)"
                     : "no loopback: either no short-wire, or PB10/PB11 path is broken");
        }
#endif

#if ESP_TX_BLINK_ON_FAIL
        ESP_TxBlink();                        /* 翻转 PB10，万用表/LED 可验证 MCU 侧 */
#endif
        return 1;                             /* 上层会打印 no module */
    }

    ESP8266_SendCmd("ATE0", "OK", 1000);      /* 关回显：响应里不再重复一遍命令，解析更干净 */

    if (ESP8266_Test() != 0) return 1;        /* 没插模块 / P2 跳线没拨对 → 1 秒内返回 */

    ESP8266_SetMode(1);                       /* 1 = STA 模式（连路由器，不是自己发热点） */

    if (strcmp(WIFI_SSID, "YOUR_WIFI_SSID") == 0 ||
        strcmp(WIFI_PWD,  "YOUR_WIFI_PWD")  == 0)
    {
        printf("[ESP8266] CONFIG ERROR: WIFI_SSID / WIFI_PWD still placeholder\r\n");
        printf("[ESP8266]   -> edit them in BSP/Inc/esp8266.h (line 25~29)\r\n");
        printf("[ESP8266]   (serial link is OK; skipping WiFi join)\r\n");
        return 5;                             /* ★ 新返回码：配置未填 */
    }

    if (ESP8266_JoinAP(WIFI_SSID, WIFI_PWD) != 0) return 2;

    ESP8266_GetLocalIP(ip);
    printf("[ESP8266] IP = %s\r\n", ip);   /* 拿到 IP 说明 DHCP 成功，是最重要的联网信号 */

#if ESP_AUTO_CONNECT_SERVER
    if (ESP8266_ConnectServer(TCP_SERVER_IP, TCP_SERVER_PORT) != 0) return 3;
#endif
    return 0;
}

uint8_t ESP8266_ConnectServer(const char *ip, uint16_t port)
{
    if (ESP8266_ConnectTCP(ip, port) != 0) return 1;
    if (ESP8266_EnterPassthrough() != 0) return 2;
    return 0;
}

/* 状态维护 */
ESP8266_Status_t ESP8266_GetStatus(void)
{
    return s_status;
}

char *ESP8266_GetLastResp(void)
{
    return s_resp;
}

/* 周期性调用：只做一件事 —— 确认 WiFi 还连着。
   ⚠️ 透传态下发 AT 会污染数据流，所以此时直接跳过不查。
   建议 5~10 秒调一次，太频繁没意义（每次要几百毫秒）。 */
void ESP8266_Process(void)
{
    if (s_passthrough) return;                /* 透传态不打扰数据流 */
    if (s_uart_taken)  return;
    ESP8266_CheckWifiStatus();
}


/**
 * @brief   把最近一次 AT 的原始响应按 "可见字符 + HEX" 打出来
 * @param   idx  第几条命令（打印用序号）
 * @param   cmd  刚发出去的命令原文（用来和响应长度做对比）
 * @param   rc   ESP8266_SendCmd 的返回码
 * @retval  无
 */
static void ESP_ProofDump(uint8_t idx, const char *cmd, uint8_t rc)
{
    uint16_t i;
    uint16_t n      = (uint16_t)strlen(s_resp);
    uint16_t cmdlen = (uint16_t)strlen(cmd);

    printf("[PROOF] %u) %-14s rc=%u len=%u",
           (unsigned int)idx, cmd, (unsigned int)rc, (unsigned int)n);

    if (rc == 4)
    {
        printf("  -> USART3 taken by protocol DMA, test skipped\r\n");
        return;
    }

    /* ★ 最关键的一条判据：
       响应长度 > 命令长度，说明除了"模块回显的命令本身"，还有【模块自己追加的字节】
       （OK / 版本号 / IP ...）。这些字节 MCU 自己造不出来，只可能来自对端。
       自发自收最多只能拿到 <= 命令长度的字节（就是自己发的那几个），超出的部分造不出来。 */
    if (n > cmdlen)
    {
        printf("  [len %u > cmd %u: EXTRA bytes -> from MODULE]",
               (unsigned int)n, (unsigned int)cmdlen);
    }
    printf("\r\n");

    if (n == 0)
    {
        printf("[PROOF]      (0 byte: totally silent)\r\n");
        return;
    }

    printf("[PROOF]      txt: \"");
    for (i = 0; i < n && i < 120; i++)
    {
        char c = s_resp[i];
        printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
    }
    printf("\"\r\n");

    printf("[PROOF]      hex: ");
    for (i = 0; i < n && i < 32; i++) printf("%02X ", (unsigned int)(unsigned char)s_resp[i]);
    printf("\r\n");
}

/**
 * @brief   链路自证：逐条发 AT 并把原始响应全打出来
 * @param   无
 * @retval  0=AT+WiFi 全通 / 1=AT 通 WiFi 不通 / 2=AT 不通 / 3=串口被协议栈占用
 */
uint8_t ESP8266_ProofLink(void)
{
    uint8_t rc;
    uint8_t idx  = 1;
    uint8_t at_ok = 0, wifi_ok = 0;
    char    ip[24];
    char    cmd[128];

    printf("\r\n======== ESP8266 LINK PROOF (raw bytes, no guessing) ========\r\n");
    printf("[PROOF] rule: real reply = echo + \"OK\" (LONGER than the command)\r\n");
    printf("[PROOF]       self-echo  = \"AT\\r\\n\" only (no OK) -> that would be FAKE\r\n");

    /* ① AT：最基础的握手。通了就证明 PB10->模块RXD 与 模块TXD->PB11 双向都活着 */
    rc = ESP8266_SendCmd("AT", "OK", 1500);
    ESP_ProofDump(idx++, "AT", rc);
    at_ok = (rc == 0);
    if (rc == 4) return 3;

    /* ② ATE0 关回显：之后的响应里不再重复命令本身，原文更干净、更好判 */
    rc = ESP8266_SendCmd("ATE0", "OK", 1000);
    ESP_ProofDump(idx++, "ATE0", rc);

    /* ③ AT+GMR 读固件版本：★ 最硬的一条证据。
          返回值是模块内部的版本串（AT version / Bin version），
          自发自收与串扰【绝对造不出来】—— 能打出来就是对端真的在说话。 */
    rc = ESP8266_SendCmd("AT+GMR", "OK", 2000);
    ESP_ProofDump(idx++, "AT+GMR", rc);

    /* ④ AT+CWMODE=1 设为 STA 模式（连路由器，不是自己发热点） */
    rc = ESP8266_SendCmd("AT+CWMODE=1", "OK", 2000);
    ESP_ProofDump(idx++, "AT+CWMODE=1", rc);

    /* ⑤ AT+CWJAP 连 WiFi：最慢的一步，认证 + DHCP 可能要十几秒 */
    ESP_Cmd(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PWD);
    rc = ESP8266_SendCmd(cmd, "OK", 15000);
    ESP_ProofDump(idx++, "AT+CWJAP", rc);
    wifi_ok = (rc == 0);

    /* ⑥ AT+CIFSR 读 IP：拿到 STAIP 说明 DHCP 成功，是"真连上网"的标志 */
    rc = ESP8266_SendCmd("AT+CIFSR", "OK", 3000);
    ESP_ProofDump(idx++, "AT+CIFSR", rc);

    /* 从 CIFSR 的响应里把 IP 抠出来 */
    ip[0] = '\0';
    {
        char *p = strstr(s_resp, "STAIP,\"");
        if (p == NULL) p = strstr(s_resp, "APIP,\"");
        if (p != NULL)
        {
            char *q = strchr(p, '"');
            if (q != NULL)
            {
                uint16_t k = 0;
                q++;
                while (*q != '\0' && *q != '"' && k < (uint16_t)(sizeof(ip) - 1))
                {
                    ip[k++] = *q++;
                }
                ip[k] = '\0';
            }
        }
    }

    /* ===== 结论：分级判，别再让"链路通"和"WiFi 通"混为一谈 ===== */
    printf("[PROOF] ------------ VERDICT ------------\r\n");
    printf("[PROOF] AT handshake : %s\r\n", at_ok   ? "OK" : "FAILED");
    printf("[PROOF] WiFi join    : %s   (SSID=\"%s\")\r\n",
           wifi_ok ? "OK" : "FAILED", WIFI_SSID);
    printf("[PROOF] IP           : %s\r\n", ip[0] ? ip : "(none)");

    if (at_ok && wifi_ok)
    {
        printf("[PROOF] >>> PROVEN: AT link UP + WiFi UP. Full link OK. <<<\r\n");
        return 0;
    }
    else if (at_ok)
    {
        /* ★ 这正是"看起来像坏了、其实链路完全正常"的那一档：
           AT 握手实实在在通了，模块在回话，串口链路没问题；
           卡住的是连热点 —— 那是 AP 侧的事（频段/加密/密码），跟串口无关。 */
        printf("[PROOF] >>> PROVEN: AT link IS UP (module really answers). <<<\r\n");
        printf("[PROOF] >>> but WiFi join FAILED. Serial link is fine, <<<\r\n");
        printf("[PROOF] >>> so fix the AP: 2.4GHz only / WPA2 / password / signal. <<<\r\n");
        return 1;
    }
    else
    {
        printf("[PROOF] >>> AT link DOWN: no valid reply (see the hex above). <<<\r\n");
        return 2;
    }
}
