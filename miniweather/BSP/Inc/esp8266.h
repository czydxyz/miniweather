/* [约束] ESP_WIFI_SSID / ESP_WIFI_PWD / ESP_TCP_IP / ESP_TCP_PORT 为占位符，必须改。
 * [约束] 只支持 2.4GHz + WPA2（不支持 WPA3）；热点属公用网络时需放行端口入站。
 * [约束] P2 跳线帽必须纵向插 GBC 侧（左 4<->6、右 3<->5）；插 RS232 侧 USART3 收不到数据。
 * [约束] 联调协议时必须拔掉 ESP8266（模块 TXD 与 USB-TTL TXD 会同时驱动 PB11）。
 */
#ifndef __ESP8266_H
#define __ESP8266_H

#include "main.h"
#include "usart.h"


#ifndef WIFI_SSID
#define WIFI_SSID           "czy123"     /* ← 改成你的 WiFi 名（2.4G，勿用中文/空格） */
#endif
#ifndef WIFI_PWD
#define WIFI_PWD            "czy321321"      /* ← 改成你的 WiFi 密码 */
#endif
#ifndef TCP_SERVER_IP
#define TCP_SERVER_IP       "192.168.137.1"      /* ← 改成 PC 的局域网 IP（ipconfig 查） */
#endif
#ifndef TCP_SERVER_PORT
#define TCP_SERVER_PORT     8888                 /* ← 改成服务器端口 */
#endif

/* 是否在初始化时就自动连服务器并进透传（0 = 只连 WiFi，不连 TCP） */
#ifndef ESP_AUTO_CONNECT_SERVER
#define ESP_AUTO_CONNECT_SERVER     1
#endif

#ifndef ESP_TX_BLINK_ON_FAIL
#define ESP_TX_BLINK_ON_FAIL        1
#endif

#ifndef ESP_LOOPBACK_PROBE
#define ESP_LOOPBACK_PROBE          1
#endif

#ifndef ESP_CAPTURE_BOOTLOG_MS
#define ESP_CAPTURE_BOOTLOG_MS      1500
#endif

#ifndef ESP_WAIT_READY_MS
#define ESP_WAIT_READY_MS           4000
#endif

#ifndef ESP_USE_HW_RESET
#define ESP_USE_HW_RESET            1
#endif

#ifndef ESP_PROOF_LINK
#define ESP_PROOF_LINK              0
#endif

/* AT 响应接收缓冲区 */
#define ESP8266_RX_BUF_SIZE   512
/* 单条 AT 指令默认超时（ms） */
#define ESP8266_CMD_TIMEOUT   2000

/* ================== 状态定义 ================== */
typedef enum
{
    ESP_WIFI_DISCONNECTED = 0,   /* 未连接 WiFi */
    ESP_WIFI_CONNECTED,          /* 已连接 WiFi */
    ESP_TCP_CONNECTED,           /* 已连接 TCP 服务器 */
    ESP_TCP_DISCONNECTED         /* TCP 断开 */
} ESP8266_Status_t;

/* ================== 函数声明 ================== */

/* ---- 初始化 / 联网 ---- */
uint8_t  ESP8266_Init(void);
uint8_t  ESP8266_ProofLink(void);
uint8_t  ESP8266_IsPassthrough(void);

uint8_t  ESP8266_ConnectServer(const char *ip, uint16_t port);
                                 /* 连 TCP 服务器 + 进透传。0 = 成功 */

/* ---- AT 指令基础 ---- */
uint8_t  ESP8266_SendCmd(const char *cmd, const char *expect, uint32_t timeout);
                                 /* 发一条 AT 指令，等到 expect 出现为止。
                                    返回 0=成功 / 1=超时 / 2=ERROR / 3=FAIL / 4=串口已被协议栈接管 */

/* ★ 交出 USART3：Comm_Init() 开完 DMA 循环接收后调用一次。
   之后本驱动再也不碰 huart3（否则轮询 Receive 会撞上 DMA 的 BUSY_RX，
   表现为 SendCmd 全部超时，还会把 Comm_Poll 卡住若干秒）。 */
void     ESP8266_HandOver(void);
uint8_t  ESP8266_Test(void);                          /* AT */
uint8_t  ESP8266_Reset(void);                         /* AT+RST */

/* ---- 排障专用（"no module" 时靠它们定位是硬件还是波特率） ---- */
void     ESP8266_BootCapture(uint32_t ms);
                                 /* ★ 尽早调用（main() 里 USART3 初始化完立刻调）：
                                    不发任何东西，只把模块这段时间吐的字节记进内部缓冲，
                                    之后由 ESP8266_Init() 打印出来。
                                    重复调用无效（只抓第一次）。 */
void     ESP8266_HWReset(void);
                                 /* 用 PF6 给模块一次硬件复位（RST 低 100ms → 高 500ms）。
                                    仅当 ESP_USE_HW_RESET=1 时才会被 Init 自动调用，
                                    也可以手工调用。 */
uint16_t ESP8266_Listen(uint32_t ms, char *out, uint16_t outlen);
uint8_t  ESP8266_AutoBaud(void);
                                 /* 波特率自适应：依次用 115200 / 9600 / 57600 /
                                    38400 / 19200 / 74880 发一次 AT，谁回了 OK 就锁谁。
                                    返回 0 = 锁定成功（波特率已写进 huart3）
                                          1 = 全都没回（模块没上电 / 线不通 / 固件不是 AT） */
void     ESP8266_DumpResp(const char *tag);
                                 /* 把最近一次 AT 收到的原始字节按 "可见字符+HEX" 打出来 */
uint8_t  ESP8266_SetMode(uint8_t mode);               /* AT+CWMODE，1=STA */
uint8_t  ESP8266_JoinAP(const char *ssid, const char *pwd);  /* AT+CWJAP */
uint8_t  ESP8266_CheckWifiStatus(void);               /* AT+CWJAP? 并刷新内部状态 */
char    *ESP8266_GetLocalIP(char ip[16]);             /* AT+CIFSR，取本机 IP */

/* ---- 数据收发 ---- */
uint8_t  ESP8266_ConnectTCP(const char *ip, uint16_t port);   /* AT+CIPSTART */
uint8_t  ESP8266_EnterPassthrough(void);              /* AT+CIPMODE=1 + AT+CIPSEND */
uint8_t  ESP8266_ExitPassthrough(void);               /* +++ 退出透传 */
uint8_t  ESP8266_SendData(const uint8_t *data, uint16_t len); /* 透传态下直接发 */
uint8_t  ESP8266_CloseTCP(void);                      /* AT+CIPCLOSE */

/* ---- 状态 ---- */
ESP8266_Status_t ESP8266_GetStatus(void);
void     ESP8266_Process(void);       /* 周期性调用：刷新连接状态（每 5~10 秒一次即可） */
char    *ESP8266_GetLastResp(void);   /* 取最近一次 AT 响应（调试用） */

#endif /* __ESP8266_H */
