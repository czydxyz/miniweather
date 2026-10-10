# MiniWeather 项目 Bug 记录与避坑清单

> 本文记录本项目从建工程到首次跑通 9/9 自检期间，**所有实际遇到并修掉的问题**，
> 以及**发现但还没修**的隐患。每条都写清楚：现象 → 根因 → 为什么原写法是错的 → 修法 → 一句话教训。
>
> 更新至 2026-09-19（含本次修的 4 个问题）。

---

## 总览

| 编号 | 层 | 一句话 | 状态 |
|---|---|---|---|
| A1 | RTOS | 注释误导：曾误判「必须手写 SysTick_Handler」，实际由 port.c 宏改名提供 | ✅ 本次修正 |
| B1 | 串口 | `ESP_UartRecover` 缺前向声明 → Keil error #159 | ✅ 已修 |
| B2 | 串口 | ORE 溢出后 HAL 状态机锁死 → 之后永远收不到数据 | ✅ 已修 |
| B3 | 串口 | 发完 AT 立刻进等待循环 → 空隙盲区丢首字节 | ✅ 已修 |
| B4 | 串口 | 用 `MX_USART3_UART_Init()` 归还引脚无效 → PB10 停在 GPIO 模式 | ✅ 已修 |
| B5 | 串口 | 协议栈接管 USART3 后 AT 驱动还在抢 → `HAL_BUSY` + 卡死 Comm_Poll | ✅ 已修 |
| B6 | 配置 | WiFi 账号是占位符，却报 "join fail" 误导 | ✅ 本次修复 |
| C1 | I2C | 外接模块没供电 → ESD 二极管拖死整条总线，板载器件一起 FALSE | ✅ 已定位（硬件） |
| C2 | 传感器 | BMP280 气压补偿用 int32 溢出 | ✅ 已修 |
| C3 | 传感器 | SGP30 命令字长度错（6→8 / 3→5 字节） | ✅ 已修 |
| C4 | 任务 | 湿度补偿用了未初始化的栈变量 | ✅ 本次修复 |
| D1 | SPI | W25Q128 ID 死磕 0xEF4018 → BY25Q128 是 0x68 | ✅ 已修 |
| D2 | Flash | 擦除前不清标志 + `Banks` 不填 → HAL_BUSY / 随机 | ✅ 已修 |
| E1 | 协议 | `AA AA 55` 边界要停留不回退 | ✅ 已修 |
| E2 | 协议 | n==0 时也必须 Proto_Tick | ✅ 已修 |
| E3 | 协议 | 三条丢弃路径：超长 / CRC 错 / 半帧超时 | ✅ 已修 |
| E4 | 协议 | default 分支必须回 0x01，否则上位机只能干等超时 | ✅ 已修 |
| F1 | LCD | MADCTL 0x48 镜像 → 0x08 | ✅ 已修 |
| F2 | LCD | 逐列字模必须每列开细窗口 | ✅ 已修 |
| F3 | LCD | `lcdfont.h` 数组只能被 1 个 .c 包含 | ✅ 已修 |
| F4 | 工程 | Keil 没加 `lcd_test.c` → L6218E | ✅ 已修 |
| F5 | LCD | 界面显示重叠：旧值残留 / 行距不统一 / 首帧没清屏 | ✅ 已重写 |
| H1 | 架构 | Boot 与 App 各写一份分区表 + `BootFlag_t`，靠注释提醒"同步改" | ✅ 本次收敛为单一真源 |
| H2 | 工程 | `boot.c` / `flash_boot.c` 直接操作寄存器却没包含 `stm32f4xx.h` | ✅ 本次修复 |
| H3 | OTA | 搬运失败后清掉升级标志 → 一次断电永久变砖 | ✅ 本次修复（P0） |
| G1 | 任务 | 读快照没进临界区（单边加锁等于没加锁） | ✅ 本次修复 |
| G2 | 任务 | `Queuesesor` 队列建了没人用 | ⚠️ 未修 |
| G3 | 任务 | `osThreadCreate` 返回值没检查 | ⚠️ 未修 |
| G4 | 系统 | `fputc` 多任务重入，日志会串 | ⚠️ 未修 |
| I1 | 硬件 | **P2 跳线插错排** → PB10/11 被 SP3232 强拉，模块在板上 0 字节 | ✅ 已定位（跳线纵向插 GBC 侧） |
| I2 | 方法论 | **相邻引脚串扰** → 模块拔掉也能自发自收 4/4，此前所有回环结论作废 | ✅ 已修（探针加 RX 下拉） |
| I3 | 方法论 | 74880 ROM 日志只在复位后 ~200ms 吐 → 抓晚了等于没抓 | ✅ 已修（紧跟 hw_reset） |
| I4 | 方法论 | PC0 是双负载网络，其电平不能反推单个器件状态（误判已撤回） | ✅ 已澄清 |
| I5 | 配置 | 热点 5GHz → `+CWJAP:3` 扫不到；WPA3 不支持 | ✅ 已修（改 2.4GHz） |
| I6 | 环境 | Windows 移动热点算公用网络 → 防火墙拦 8888 入站 | ✅ 已修（放行规则） |
| I7 | 假象 | 透传后 AT 不再被解析，只回 `>`（hex 尾 3E）→ ProofLink 全 FAILED | ✅ 已修（置 0） |
| I8 | 协议 | **AI 文本旁路吃掉长帧分片** → WiFi OTA 每块重试 2~3 次甚至失败 | ✅ 已修（加解析器空闲判据） |
| I9 | 应用 | 服务端 `ACK:` 回执被当成 AI 建议显示 | ✅ 已关（注释掉 ACK 回执） |
| I10 | 系统 | **OTA 期间 AI 上报抢 USART3** → 应答被背压堵 5.8s，升级在第 223 块中止 | ✅ 已修（OTA 期间停报 + 超时 1.5→3s） |

---

# 一、RTOS 与启动层

## A1. SysTick 注释误导（本次修正 —— 这是一次「误诊」留下的坑）

**当时的（错误）说法**：

> `SysTick_Handler` 根本不存在，`osKernelStart()` 之后会立刻卡死，必须在 `stm32f4xx_it.c` 里手写：
> ```c
> void SysTick_Handler(void) { osSystickHandler(); }
> ```

**真实情况**：这个诊断是**错的**。系统之所以一直正常在跑，是因为 CubeMX 用了「宏改名」技巧：

```c
/* Core/Inc/FreeRTOSConfig.h:133 —— 那不是注释，是宏 */
#define xPortSysTickHandler SysTick_Handler
```

```c
/* Middlewares/.../portable/RVDS/ARM_CM4F/port.c:501 */
void xPortSysTickHandler( void )     /* ← 预处理后变成 void SysTick_Handler(void) */
```

`port.c` 也 include 了 `FreeRTOSConfig.h`，所以它的函数在预处理阶段就被改名为 `SysTick_Handler`，链接时自然成为中断向量入口。同一文件里还有另外两条改名：

```c
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
```

**为什么误诊**：三条线索都指向"没人定义 SysTick_Handler"

1. CubeMX 把 HAL 时基改成 TIM2，所以 `stm32f4xx_it.c` 里确实没生成这个函数
2. 正点原子 `delay.c:101` 里那个 `SysTick_Handler` 被 `#if SYS_SUPPORT_OS` 包着，而 `delay.h` 里**根本没定义这个宏**（未定义 → 按 0 处理）→ 整块被裁掉
3. 全文 grep "SysTick_Handler" 只能搜到 `FreeRTOSConfig.h` 里那一行，看上去像注释

第 3 条是查的重点对象，但当时把它当注释略过了 —— 宏改名这种间接提供符号的方式，靠搜函数名是搜不到"定义"的。

**误诊的代价有多大**：如果照着那段注释真去 stm32f4xx_it.c 补一个

```c
void SysTick_Handler(void) { osSystickHandler(); }
```

会和 `port.c` 改名来的那个**同名强符号撞车** → Keil 报 `L6200E: Symbol SysTick_Handler multiply defined`。

**本次做法**：把 `stm32f4xx_it.c` 里那段注释改写成了事实说明，并在里面明确写出"别在这补名字"。

> **一句话教训：错误的注释比没注释更危险。** 新人读到"★★★心脏★★★ 必须手写"会立刻动手，然后撞一个莫名其妙的链接错误。
> 记结论前先验证：既然板子 FreeRTOS 明明在跑，节拍就一定有来源 —— "它没定义"这个判断和事实矛盾，矛盾就是证据。

**顺便说明**：`delay.c` 那份被裁掉反而是对的 —— 它里面调的是 `OS_CPU_SysTickHandler()`，那是 **uC/OS-II** 的函数，本工程用的是 FreeRTOS，那份实现暴露出来只会编译报错。

---

# 二、串口 / ESP8266 层（本项目的重灾区）

这层一共抓到 6 个 bug，而且互相伪装 —— 表面上都是"模块没反应"，实际是 5 个完全不同的原因。按发现顺序讲。

## B1. 缺前向声明导致 Keil error #159

**报错**：

```
..\BSP\Src\esp8266.c(162): error:  #159: declaration is incompatible
      with previous "ESP_UartRecover"  (declared at line 104)
```

**根因**：`ESP_UartRecover()` 定义在 300 行，但 104 行的 `SendCmd()` 就在调用它 —— **调用出现在定义之前**。

ARMCC 遇到没见过的函数调用，会按 C89 老规矩**隐式声明**成 `int ESP_UartRecover(...)`（返回 int）。等翻到 300 行发现真实定义是 `static void ESP_UartRecover(void)`，返回类型和链接属性都对不上，于是报错。

**修法**（`esp8266.c:116`）：

```c
static void ESP_UartRecover(void);   /* 前向声明 */
```

> **一句话教训**：这类问题在 PC 的 gcc 上通常只是警告，ARMCC 默认当错误处理 —— 所以**本地 gcc 试编译通过不代表 Keil 能过**，务必在 Keil 里再过一遍。

---

## B2. ORE 溢出后 HAL 状态机锁死（最阴的一个）

**现象**：第一次上电收到 1 个字节 `'r'`（`ready` 的首字母），之后六个波特率各发一次 `AT`，**一次都没回**。看起来像"模块坏了 / TX 线断了"。

**两层原因**

**第一层 —— 数据被 MCU 自己错过了**
MCU 上电先要初始化 LCD、三个传感器、Flash，折腾掉约 1 秒。而模块早就吐完 `ready\r\n` 了。那会儿没人读接收寄存器，第二个字节（`e`）到达时触发 **ORE（OverRun Error）溢出置位**，后面的字节全丢，只剩 DR 里那个 `'r'` 等着我们捞。

**第二层（致命的）—— 串口被锁死了**
ORE 挂住之后，HAL 内部的 `RxState` / `gState` 卡在非 READY。**从这一刻起，每一次 `HAL_UART_Receive` 都直接返回错误。**

所以后面 AutoBaud 发 6 次 AT 全是白忙 —— 不是模块不回，是我们压根没在听。

> 症状是"模块没反应"，真凶是 MCU 自己把自己锁了。

**修法**（`esp8266.c:300`）：

```c
static void ESP_UartRecover(void)
{
    volatile uint32_t tmp;

    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_ORE)) {
        /* ① 清 ORE：先读 SR 再读 DR（顺序由硬件规定） */
        tmp = huart3.Instance->SR;
        tmp = huart3.Instance->DR;
        __HAL_UART_CLEAR_OREFLAG(&huart3);
    }
    /* ② 把 DR 里剩余的字节读空，别再留残骸 */
    while (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) {
        tmp = huart3.Instance->DR;
    }
    /* ③ 复位 HAL 的状态字段 —— 缺这步前面的清理就白做了 */
    huart3.ErrorCode = HAL_UART_ERROR_NONE;
    huart3.RxState   = HAL_UART_STATE_READY;
    huart3.gState    = HAL_UART_STATE_READY;

    (void)tmp;
}
```

在 `ESP8266_Listen()` 和 `ESP8266_SendCmd()` 的开头各调一次。

> **一句话教训**：串口"能收一个字节然后彻底哑掉" = ORE + HAL 状态机锁死。**先别怀疑对端硬件**，先把自己的状态机救回来。

---

## B3. 发送后立刻等待 → 空隙盲区丢首字节

**现象**：短接 PB10↔PB11 做自发自收，期望看到 `AT\r\n` 回来，结果只收到**一个 `'A'`**（`hex: 41`）。

**根因**

发 4 个字节要三百多微秒。这段时间 MCU 正在**阻塞发送**，一个字节都没读接收寄存器。

```
 send "AT\r\n"  ─────────────►  300+ us，全程不读 DR
 recv            ▼'A'         ▼'T'  ← 第二个字节一到，ORE 触发，后面全丢
```

**这个坑有多阴**：如果模块真的回了 `OK\r\n`，而它回得特别快（比如回显开着、紧跟 AT 就到），我们可能只捞到一个 `'O'`。于是：

```c
strstr(s_resp, "OK")   /* 永远匹配不上 */
```

**明明通信成功了，却被判成超时。**

**修法**（`esp8266.c`，`ESP_SendStr("\r\n")` 之后、进入等待循环之前）：

```c
/* 先把发送期间陆续到达的字节全收进缓冲 */
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

/* 抢救到的字节里可能已经有答案了，先判一次再进等待循环 */
if (i > 0 && expect == NULL) return 0;
if (expect != NULL && strstr(s_resp, expect) != NULL) return 0;
```

> **一句话教训**：阻塞发送期间接收是**无人值守**的。凡是"发完立刻等响应"的代码，都要先做一次冲刷回收，再进入超时循环。

---

## B4. `MX_USART3_UART_Init()` 归还引脚无效

**背景**：为了方便查线，写了个 `ESP_TxBlink()`：把 PB10 改成普通 GPIO 翻转若干次，测完再恢复成 USART3。

**错在哪**

```c
MX_USART3_UART_Init();     /* ✗ 看起来天经地义，其实是无效的 */
```

`HAL_UART_Init()` 内部只有看到 `huart->gState == HAL_UART_STATE_RESET` 时才会去调用 `HAL_UART_MspInit()` —— 而我们此刻的 `gState` 是 `READY`（外设早就初始化过了）。

结果：**只重配波特率，不重配 GPIO**。PB10 永久停在 GPIO 输出模式，`USART3` 从此发不出任何数据。

这是 HAL 库一个非常反直觉的设计：同一个 Init 函数，在不同状态下做的事情不一样。

**修法**（`esp8266.c:385`）：

```c
/* 先把状态强行按回 RESET，逼 MspInit 重跑一遍（GPIO/DMA/NVIC 全恢复） */
huart3.gState    = HAL_UART_STATE_RESET;
huart3.RxState   = HAL_UART_STATE_RESET;
MX_USART3_UART_Init();
```

同时把 `ESP_TX_BLINK_ON_FAIL` 默认改成 **0**（短接测试已证明 MCU 发送正常，不必每次多等 5 秒）。

> **一句话教训**：想让 HAL 的 `xxx_Init()` 重新干活，先把它按回 `RESET`。

---

## B5. 协议栈接管 USART3 后，AT 驱动还在抢

**现象**：`Comm_Init()` 之后，设备隔几秒卡一下、偶尔丢应答。

**根因**

`Comm_Init()` 启动了 USART3 的 DMA 循环接收 → `huart3.RxState` 变成 `BUSY_RX`。
此时 AT 驱动的 `HAL_UART_Receive()`（轮询）**直接返回 `HAL_BUSY`**。

而 `ESP8266_Process()` 原来的守卫只有一条：`if (s_passthrough) return;`。
只要模块没配成功（`s_passthrough == 0`），它每 5 秒就真的去发一次 `AT+CWJAP?`，白等 2 秒超时。这 2 秒里 `Comm_Poll` 停摆 → DMA 缓冲积压 → 丢帧。

**修法**：显式"交出"机制

```c
/* esp8266.c */
static uint8_t s_uart_taken = 0;
void ESP8266_HandOver(void) { s_uart_taken = 1; }

/* SendCmd 开头 */
if (s_uart_taken) return 4;              /* 新增返回码 4 */

/* Process 开头 */
if (s_uart_taken) return;                /* 串口已归协议栈，别碰 */

/* main.c */
Comm_Init();
ESP8266_HandOver();                      /* 顺序固定，不可交换 */
```

> **一句话教训**：同一外设被两个模块分时使用时，**交接必须是显式的**。"我以为它不会调"不算保护。

---

## B6. WiFi 账号是占位符，却报 "join fail"（本次修复）

**现象**：

```
[ESP8266] WiFi join fail (check SSID/PWD)
```

**问题在哪**：`esp8266.h` 里的默认值一直是占位符

```c
#define WIFI_SSID   "YOUR_WIFI_SSID"
#define WIFI_PWD    "YOUR_WIFI_PWD"
```

不改就去联网必然失败，但报出来的信息指向"**SSID/密码不对**"。于是顺着"密码错"的方向去查 —— 查频段、查信号、查特殊字符 —— 实际是**压根没配**。

串口链路这时候其实完全正常，属于**配置层**问题，不该和网络层共用同一个错误提示。

**修法**：Init 里在联网之前加占位符自检（返回码 **5**）

```c
if (strcmp(WIFI_SSID, "YOUR_WIFI_SSID") == 0 ||
    strcmp(WIFI_PWD,  "YOUR_WIFI_PWD")  == 0)
{
    printf("[ESP8266] CONFIG ERROR: WIFI_SSID / WIFI_PWD still placeholder\r\n");
    printf("[ESP8266]   -> edit them in BSP/Inc/esp8266.h (line 25~29)\r\n");
    printf("[ESP8266]   (serial link is OK; skipping WiFi join)\r\n");
    return 5;
}
```

`main.c` 里同步加了分支：

```c
(ret == 5) ? "CONFIG NOT SET -> edit WIFI_SSID/WIFI_PWD in esp8266.h" :
```

> **一句话教训**：**默认配置要能被机器检测出来**。占位符留到运行时才发现，是把本该 compile time 暴露的问题推迟到了最贵的排查环节。

### ESP8266 返回值速查表（改后）

| 返回值 | 含义 | 属于哪一层 |
|---|---|---|
| 0 | 全程成功（已进入透传） | — |
| 1 | 串口层失败：没模块 / AT 无回应 | **链路层**（P2 跳线、供电、TX/RX 交叉） |
| 2 | WiFi 连不上 | **网络层**（SSID/密码、非 2.4G、信号） |
| 3 | TCP 连不上 | **网络层**（服务器没开、IP/端口、AP 隔离） |
| 4 | 串口已被协议栈接管 | **使用错误**（忘了 HandOver） |
| 5 | 配置没填（占位符） | **配置层** |

> 排障顺序固定：**先确认 1 消失，再谈 2 和 3。** 分层看这条返回码，能少走很多弯路。

---

## B7. 两个 TXD 同时驱动 PB11（2026-09-24 一天内复发两次，同一物理冲突两种假象）

**USART3 的 PB10/PB11 是独占资源：PB11 上只允许有一个 TX 驱动源。**
ESP8266 的 TXD 和 USB-TTL 的 TXD 都往 PB11 上挂时，谁把线摁在 idle 高电平，
另一方就永远拉不出起始位 —— 表现为**"一个字节都收不到"**。

### 两次现场

| 时间 | 板上的配置 | 表现 | 假象 |
|---|---|---|---|
| 09-24 00:20 | ESP8266 + USB-TTL **同时在板** | 板→PC 通、**PC→板 不通**；RX 日志里能看到 `41 54 0D 0A`（ASCII `AT\r\n`） | 看着像"线没接对 / 跳线没拨" |
| 09-24 02:09 | 同上，换成测 ESP | `[ESP8266] bootlog: 0 byte(s)` + `AT: 0 byte(s)` → `DIAG: RX+TX both silent` | 看着像"模块坏了 / 没上电" |

**同一个冲突，两个完全不同的假象** —— 这就是它值得单独一条的原因。

### 代码里已经内置了判别（`esp8266.c:659-663`）

```c
if (n) printf("[ESP8266] DIAG: RX alive, TX dead -> P2 needs BOTH jumpers on GBC side\r\n");
else   printf("[ESP8266] DIAG: RX+TX both silent -> check module VCC / P2 jumper / TX-RX cross\r\n");
```

- `RX alive, TX dead`：bootlog 有字节但 AT 没回 = **单向通** → 99% 是 **P2 的两根跳线帽没"成对"拨到 GBC 侧**（只拨了一根就会这样）
- `RX+TX both silent`：双向全静默 → 模块没上电 / P2 两根都没拨 / TX-RX 没交叉 / **PB11 被 USB-TTL 摁住**

### 铁律

> **USB-TTL 和 ESP8266 永远不能同时在板。**
> 测协议/OTA → 插 USB-TTL、**拔 ESP8266**；
> 测 WiFi/TCP → 插 ESP8266、**拔 USB-TTL（TXD/RXD/GND 三根全拔）**。
> 换插之后**必须重新上电**：`ESP8266_Init()` 只在 `main.c:293` 跑一次，
> `ESP8266_Process()` 只查状态不重连（开头就是 `if (s_uart_taken) return;`）。

### ⚠️ 别和 B4 搞混（两者症状像，性质完全不同）

| | **B4** | **B7** |
|---|---|---|
| 坏的是谁 | **软件**把 PB10 留在 GPIO 模式 | **外部接线**冲突 |
| 坏几条线 | 只坏 **TX（PB10）** | **TX + RX 双向** |
| bootlog（纯听 RX） | **仍能收到字节** | **0 字节** |
| 触发条件 | `ESP_TxBlink()` 跑过（宏为 1 时） | 两个 TX 挂同一根线 / 跳线没拨 |
| 怎么修 | 按回 `HAL_UART_STATE_RESET` 再 `MX_USART3_UART_Init()` | 拔掉多余的那个设备 |

**判读口诀：bootlog 也是 0 = RX 也坏了 = 不是软件把引脚弄坏，是线/电/跳线。**

---

# 三、I2C 与传感器层

## C1. 外接模块没供电，拖死整条总线

**现象**：自检 4/9 —— SHTC3 / BMP280 / SGP30 / **AT24C02** 全部 FAIL。

**关键判据**

AT24C02 是**板载器件**，直接焊在 PB8/PB9 上，一根线都没接过。连它都 FAIL，说明问题不在"某个传感器接没接对"，而在**总线本身**。

四个器件（`0x70` / `0x76` / `0x58` / `0x50`）分属三个不同模块，不可能同时坏。唯一共同点就是 I2C1。

**根因**：某个外接模块 3.3V 没接。它的 SDA/SCL 引脚会通过**内部 ESD 保护二极管**从总线取电，把总线钳在低电平 → **板载器件一起完蛋**。症状极具迷惑性，看起来像板载芯片坏了。

**定位方法（30 秒）**：**拔掉所有外接模块，只留板载 AT24C02**
- 变 PASS → 就是某个外接模块的问题 → 一个一个插回去，插哪个开始 FAIL 就是哪个
- 仍 FAIL → 查软件（引脚复用、总线是否已被拉低）

实测：拔掉后立刻变 PASS，逐个插回后恢复到 8/9（剩一项是 W25Q128 误判，见 D1）。

**本板 I2C 地址基线**（`[SCAN]` 输出，以后接线问题直接对照）：

```
0x50 = AT24C02    EEPROM（板载）
0x58 = SGP30      空气质量
0x70 = SHTC3      温湿度
0x76 = BMP280     气压
0x10 = ES8388     板载音频 CODEC（没用到）
0x12 = QMA6100P   板载加速度计（没用到）
```

> **一句话教训**：**板载器件也 FAIL = 总线级故障。** 排查第一动作永远是"拔掉所有外接模块"。

---

## C2. BMP280 气压补偿 int32 溢出

**错在哪**：BMP280 官方补偿公式的中间值可以超过 32 位整数范围，用 `int32_t` 会静默截断，气压读数周期性乱跳。

**修法**（`bmp280.c:154`）：

```c
int64_t  var1, var2, p;      /* ★ 必须 64 位 */
...
var1 = ((int64_t)t_fine) - 128000;
var2  = var1 * var1 * (int64_t)dig_P6;
```

> **一句话教训**：照抄厂商公式时，**看清楚它示例代码里的变量类型**。IIR/FIR/补偿类公式的中间量往往比结果宽得多。

---

## C3. SGP30 命令字长度错误

**错在哪**：SGP30 的命令是**两字节，且每字节后面跟一个 CRC**。

| 操作 | 错误 | 正确 |
|---|---|---|
| 测量命令 `0x2008` | 6 字节 | **8 字节** |
| 设置绝对湿度 | 3 字节 | **5 字节** |

**修法**（`sgp30.c:256` / `sgp30.c:312`）：`HAL_I2C_Master_Transmit(..., 8, 100)` / `(..., 5, 100)`

> **一句话教训**：带 CRC 的传感器，命令长度 = 命令字字节数 × 2。

---

## C4. 湿度补偿用了未初始化的栈变量（本次修复）

**错在哪**（`freertos.c`，`StartTasksensor`）：

```c
SHTC3_Data_t  th;            /* 栈变量，未初始化 */
...
if (SHTC3_Read(&th) == 0) { ... } else { printf("read fail"); }
...
if ((sec % 10u) == 0u && snap.humi_01 != 0u) {
    SGP30_SetAbsoluteHumidity(th.humidity, th.temperature);   /* ✗ 可能是垃圾值 */
}
```

判据 `snap.humi_01 != 0` **挡不住这个问题**：SHTC3 一直失败时，`snap` 里可能还留着上一轮的值（非零），而 `th` 是**从未被赋值过的栈空间**，装的是上一次函数调用的残骸。

后果不是崩溃，而是往 SGP30 写了一个随机的湿度值 → 基线算法被带偏 → 读数长期漂移，极难归因。

你的日志里 `[SHTC3 ] read fail` 是出现过的，所以这条路径确实会被走到。

**修法**（本次）：

```c
SHTC3_Data_t    th  = {0};   /* ★ 必须初始化 */
BMP280_Data_t   bp  = {0};
SGP30_Data_t    air = {0};
uint8_t         th_valid = 0;    /* SHTC3 至少成功读过一次才允许做补偿 */
...
} else {
    printf("[SHTC3 ] read fail\r\n");
}
...
/* 判据必须是 "真的读到过"，不能是 "值非零" */
if ((sec % 10u) == 0u && th_valid) {
    SGP30_SetAbsoluteHumidity(th.humidity, th.temperature);
}
```

> **一句话教训**：**"读成功了吗"必须用一个显式的布尔量记录**，不要用"值是否等于某数"来反推。局部变量一律初始化。

---

# 四、SPI 与 Flash 层

## D1. W25Q128 ID 死磕 0xEF4018

**现象**：`[5] W25Q128 id=0x684018 FAIL`

**分析**：后两字节 `4018`（类型 + 容量字节）**完全正确**，这说明 SPI 通信是好的。首字节 `0x68 ≠ 0xEF` 是因为这颗是**比亚迪 BY25Q128**（国产兼容片），不是华邦原装。

- 真接线错会读到 `0x000000` 或 `0xFFFFFF`，不可能后两字节分毫不差

**修法**：判定从"全等于"改成"容量字节对了、厂商字节不是无效值"

```c
/* w25q128.h */
#define W25Q_MID_WINBOND     0xEF   /* 华邦 */
#define W25Q_MID_BOYA        0x68   /* 比亚迪（本板实测） */
#define W25Q_MID_GIGADEVICE  0xC8   /* 兆易 */
#define W25Q_TYPE_CAP_16MB   0x4018

#define W25Q128_ID_IS_VALID(id) \
    ((((id) & 0x00FFFFu) == W25Q_TYPE_CAP_16MB) && \
     (((id) >> 16) != 0x00u) && (((id) >> 16) != 0xFFu))
```

并新增 `W25Q128_IDName()` 返回 `"BY25Q128"`，串口和 LCD 直接打印真实芯片名。

> **一句话教训**：**ID 判断要分清"型号字段"和"制造商字段"**。兼容片满天飞，死磕厂商字节只会把良品判成废品。
> 这次 W25Q128 被判 FAIL，实际是判断逻辑太严，不是硬件故障 —— 而且它反而从侧面证明了 SPI 是通的。

---

## D2. STM32F4 内部 Flash 两个坑

（均已处理，`flash_if.c:93` / `:100` / `:153`）

**坑一：擦除前必须清标志**

```c
__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP    | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                       FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
HAL_FLASH_Unlock();
/* 然后才是 Erase */
```

不清的话第二次擦除直接返回 `HAL_BUSY`（它以为上一次还没完）。

**坑二：`erase.Banks` 必须显式填**

```c
erase.Banks = FLASH_BANK_1;      /* F407 只有一个 Bank */
```

扇区擦除时 HAL 其实不看这个字段，但不填它是**未初始化的值**。一旦开了 `USE_FULL_ASSERT`，assert 会拿它去比较 → 随机失败。典型的"能跑但埋雷"。

> **一句话教训**：结构体里的字段**一个都不要省**。特别是 HAL 的 Init/Conf 类结构，"这个参数用不上"往往是错觉。

---

# 五、协议层（第 9 / 10 / 11 节）

框架本身没出过 bug，但这四条是当初设计时就埋进去的防御，列出来说明它们各自对应什么真实故障。

## E1. `AA AA 55` 边界必须停留不回退

```c
case P_ST_HEAD_LO:
    if (b == 0x55)      ctx->state = P_ST_HEAD_HI2...;
    else if (b == 0xAA) /* stay in P_ST_HEAD_LO */ ;   /* ★ 不能退回 HEAD_HI */
    else                ctx->state = P_ST_HEAD_HI;
```

收到第二个 `A A` 时如果退回 `HEAD_HI`，连续序列 `"AA | AA 55"` 就会被误判成"帧头错"，整帧丢失。停留在本状态继续等 `0x55` 才是正确的。

## E2. n == 0 时也要 `Proto_Tick`

`UartRx_Take()` 返回 0（这一轮没收到字节）时也必须调用 `Proto_Tick()` 推进状态机，
**否则残留的半帧永远不会被超时清理**，后续所有正确帧都被当成垃圾吃掉。
表现为"断过一次线之后再也连不上" —— 重启就好了，所以极难复现定位。

## E3. 三条丢弃路径

| 条件 | 处理 |
|---|---|
| 长度字段 > 256 | 丢弃 + 复位状态机 |
| CRC16 校验失败 | 丢弃 + 复位状态机 |
| 已经开始收但 200 ms 没有新字节 | 超时复位 |

## E4. default 分支必须回错误码

`Comm_OnFrame()` 的 switch 里，`default` 也要构造应答帧并填错误码 `0x01`（未知命令）。否则上位机发了一条不认识的命令，只能干等到超时 —— 分不清是"命令不支持"还是"设备死了"。

**协议错误码表**：

| 错误码 | 含义 |
|---|---|
| 0x00 | OK |
| 0x01 | 未知命令 |
| 0x05 | Flash 操作失败 |
| 0x08 | CRC32 不符（固件传输出错） |

---

# 六、LCD 层

| 编号 | 问题 | 修法 |
|---|---|---|
| F1 | `MADCTL(0x36)` 填 0x48 → MX=1 左右镜像，字是反的 | 改 **0x08**（`lcd_ex.c:58`） |
| F2 | `asc2_1608` 是**逐列式**字模，整块写会糊成一团 | ShowChar 必须每列开「宽1 高16」细窗口 ×8 次 |
| F3 | `lcdfont.h` 里放大数组，被多个 .c 包含 → 重复定义 | 只允许 1 个 .c 包含它 |
| F4 | Keil 工程没加 `lcd_test.c` → `L6218E` | BSP 组 Add Existing Files |

关于 F4 还有一句：**每新增一个 .c 都要记得加进 Keil 工程**，不然报的是"符号未定义"，很容易误以为代码写错了。

---

## F5. 界面显示"重叠"（本次重写）

**现象**：主界面上字符叠在一起、看起来像两个字打在一处。

**三个成因（都不是 LCD 驱动的问题，是布局写法的问题）**

**成因一 —— 旧值比新值长，多出来的字符不会被擦掉（最主要）**

`LCD_ShowString()` 只画给定字符串的像素，**不会去擦右边**。所以：

```
上一帧: "WIFI"     → 画在 x=80..111
这一帧: "OFF"      → 只画 x=80..103
                                   ↑ x=104..111 那个 'I' 原封不动留着
屏上实际显示: "OFFI"   ← 这就是"重叠"
```

温度从 `27.3` 掉到 `9.8` 同理，末尾会残留一个数字。这类残留特别容易被误判成"字模坏了"。

**成因二 —— 行距没有统一规则，靠手写常量，容易算错**

字高 16 像素，而之前的 y 坐标是 32 / 52 / 72 / 92 / 112 / 146 / 166 / 186 / 206 / 236 / 262 / 288 / 304 —— 间隔在 20 和 30 之间跳，纯手算，加一行就容易撞。

**成因三 —— 首帧没清屏**，GRAM 里的上电残留和界面混在一起。

**修法：三条机制**

**① 固定宽度文本输出（核心机制）**

```c
/* 本界面【唯一】的文本出口 */
static void UI_Text(uint16_t x, uint16_t y, const char *str, uint8_t width,
                    uint16_t fc, uint16_t bc)
{
    char    buf[32];
    uint8_t i = 0u;

    if (width == 0u) { LCD_ShowString(x, y, str, fc, bc); return; }

    while (*str != '\0' && i < width) buf[i++] = *str++;   /* 超长截断 */
    while (i < width) buf[i++] = ' ';                      /* ★ 不足补空格 */
    buf[i] = '\0';

    LCD_ShowString(x, y, buf, fc, bc);
}
```

关键在于**空格是用背景色画出来的** —— 补空格就等于把旧字符盖掉，残留从根上消失。

**② 行槽化：y 坐标不许手写**

```c
#define UI_TOP     26u
#define UI_LINE_H  19u                                  /* ★ 必须 > 字高 16 */
#define UI_ROW(n)  ((uint16_t)(UI_TOP + (n) * UI_LINE_H))
#define UI_ROW_MAX ((LCD_H - UI_TOP) / UI_LINE_H)        /* (320-26)/19 = 15 行 */

typedef enum { ROW_TEMP = 0, ROW_HUMI, ..., UI_ROW_COUNT } UI_Row_t;

/* 编译期检查：行数超屏直接编译失败，而不是到板子上才发现画不出来 */
typedef char ui_row_overflow_check[(UI_ROW_COUNT <= UI_ROW_MAX) ? 1 : -1];
```

`(void)行数` 之外的写法：这个 `typedef` 数组长度为负时会触发编译错误，**不占任何 RAM**，比运行时判断便宜得多。

**③ 首帧全屏清一次**

```c
static uint8_t first_frame = 1u;
if (first_frame) { LCD_Fill(0, 0, LCD_W - 1u, LCD_H - 1u, BLACK); first_frame = 0u; }
```

注意用 `LCD_W - 1` 而不是 `LCD_W`：`LCD_Fill(x1,y1,x2,y2)` 是**闭区间**，传 240 会越界一列。`LCD_Fill` 内部有裁剪所以不会崩，但边界值写对了更清楚。

**顺带简化了版面**：设备状态由 4 行（Window/Fan/Alarm/Manual）压成 2 行**两列并排**，整体行数从 13 行降到 13 行但行距统一、留白均匀。新布局：

```
        Temp      27.3  C
        Humi      74.7  %RH
        Press    99232  Pa
        CO2eq      400  ppm
        TVOC         0  ppb
        ────────────────────
        Window    OPEN   Fan   ON
        Alarm     NO     Man   NO
        ────────────────────
        WiFi      OFF
        Up     00:00:12

        K0:open  K1:close
        K2:beep  KUP:fan
```

**验证方式**：写了个 Python 脚本按同样参数算出所有文本块的像素矩形，**29 个块两两做重叠检测 + 越界检测**，全部通过；并渲染了 `LCD界面布局预览.png` 预览图（见项目根目录）。

> **一句话教训**：**嵌入式点阵屏没有"重绘"概念**，像素写上去就在那儿。所有会变长度的文本，一律固定宽度输出 ——
> 这条规则适用于段码屏、点阵屏、任何不带 GUI 框架的显示。
> 另外：`LCD_Fill(x1,y1,x2,y2)` 这类闭区间 API，边界一律用 `W-1`，别用 `W`。

---

# 七、FreeRTOS 任务协作层

## G1. 读快照没进临界区（本次修复）

**错在哪**：写的一方老实进临界区，读的一方直接裸拷

```c
/* comm.c —— 写（有锁） */
void Comm_UpdateSensor(const SensorSnapshot_t *snap) {
    COMM_ENTER_CRITICAL();
    g_comm.sensor = *snap;
    COMM_EXIT_CRITICAL();
}

/* app.c:385 / app.c:526 —— 读（无锁）✗ */
s = Comm_GetCtx()->sensor;     /* 注释还写着"快照是临界区里写好的" */
```

**单边加锁等于没加锁。** 临界区保护的是"**一次完整拷贝不被打断**"，只保护写不保护读，读拷贝到第 10 字节时被抢占，拿到的照样是"温度是新的、气压还是上轮"的混合体。

`TaskDisplay` 优先级 AboveNormal(4) **低于** `TaskSensor` 的 High(5)，所以抢占随时可能发生。

**修法**：补一个读接口，两边用同一把锁

```c
/* comm.h */
void Comm_GetSensorSnapshot(SensorSnapshot_t *out);

/* comm.c */
void Comm_GetSensorSnapshot(SensorSnapshot_t *out)
{
    if (out == NULL) return;
    COMM_ENTER_CRITICAL();
    *out = g_comm.sensor;
    COMM_EXIT_CRITICAL();
}

/* app.c —— 两处替换 */
Comm_GetSensorSnapshot(&s);
```

> **一句话教训**：跨任务共享的>1 字节数据，一律通过**函数接口**读写。禁止任何地方直接 `xxx_GetCtx()->field` —— 那条路径可以绕过锁，而且别人 review 也看不出来。

---

## G2 ~ G4：发现 → 2026-09-23 全部修完

| 编号 | 问题 | 后果 | 状态 |
|---|---|---|---|
| G2 | `Queuesesor` 队列建了没人用（`freertos.c:144`） | 白占 heap，还误导后来人以为数据走队列 | ✅ 已修：加 `USE_SENSOR_QUEUE` 开关，默认 0 不创建 |
| G3 | `osThreadCreate` 返回值没检查（`freertos.c:161~173`） | heap 不足返回 NULL，**任务静默不跑** | ✅ 已修：`CHECK_TASK()` 宏，失败先打印任务名再 `Error_Handler()` |
| G4 | `fputc` 走阻塞 `HAL_UART_Transmit`，多任务无互斥 | 并发 printf 丢字符 / 串成一团 | ✅ 已修：调度器启动后进临界区发送 |

### G3 详解 —— 最值得记住的一条

**为什么它阴险**：`osThreadCreate()` 失败**不报错、不崩溃、不进 HardFault**。
它只是安静地返回 NULL。于是那个任务一次都没跑过，
你的现象是"传感器没数据 / LCD 不刷新 / 收不到帧"，
你会去查驱动、查协议、查接线 —— 唯独想不到"它根本没启动"。

**改法**（`freertos.c` PM 区 + 每个创建后面）：

```c
#define CHECK_TASK(h, name)  do {                                             \
        if ((h) == NULL) {                                                    \
            printf("[FATAL] 任务 %s 创建失败：FreeRTOS heap 不足\r\n", (name));\
            Error_Handler();                                                  \
        }                                                                     \
    } while (0)

tasksensorHandle = osThreadCreate(osThread(tasksensor), NULL);
CHECK_TASK(tasksensorHandle, "tasksensor");
```

> **一句话教训**：**凡是"会失败但失败得很安静"的 API，都必须查返回值。**
> 静默失败比崩溃贵十倍 —— 崩溃有现场，静默没有。
> 同类：`osMessageCreate`、`osMutexCreate`、`xQueueCreate`、`pvPortMalloc`。

### G4 详解

`printf` 是**逐字符**调用 `fputc` 的。两个任务同时打印：

```
① 并发改写 huart1.gState → 一个返回 HAL_BUSY，字符被悄悄丢掉
② 字符串交错："[SEN[COMM] reSOR] T=25.6cv 128"
```

改法：调度器启动后用临界区包住单字节发送（115200 下 87us，远小于 1ms 节拍）。
⚠️ 它只保证**字节不丢**，不保证**整串连续** —— 需要整串完整时调用方自己包
`taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()`。
⚠️ 中断里**绝对不要** printf。

另外 `configTOTAL_HEAP_SIZE = 15360`，四个任务栈吃掉约 9.3 KB，剩 6 KB 给 lwIP —— 偏紧。建议**关掉 lwIP**（本项目不需要，功能与 Wi-Fi 完全重叠），或者把 heap 调到 24~32 KB。

---

# 八、可复用的排障方法论

这轮最大的收获不是那几个 bug，是**分层二分法**：每次遇到"不通"，先判断它在哪一层，然后只在那一层找。

```
按键/屏正常吗 → 系统层 OK
        │
 USART1 打印正常吗 → 时基、时钟 OK
        │
 SPI(LCD/W25Q) 正常吗 → 排除电源/总线整体问题
        │
 I2C 正常吗 → 板载器件也 FAIL ？总线级 : 器件级
        │
 USART3 有 bootlog 吗 → RX 通
        │
 USART3 有 AT 回应吗 → TX 通
        │
 WiFi 连上了吗 → 凭据/频段问题
        │
 TCP 连上了吗 → 服务器/网络问题
```

几个反复验证过的判据：

1. **板载器件也挂 = 总线级故障**，第一动作永远是"拔掉所有外接模块"
2. **能收 1 个字节然后全哑 = ORE + HAL 状态机锁死**，先救自己别怪对端
3. **自发自收能收到 = MCU 侧发送硬件是好的**，问题在这条线出去之后
4. **读到 ID 的后半段正确 = 通信本身好**，多半是识别逻辑太严
5. **`localize` 不了就想能不能让它自己说话** —— 本项目加的 `[SCAN]` I2C 扫描、`DIAG: RX alive, TX dead`、占位符自检，都是为了让下一次问题能够"一上电就自报家门"

> **最省时间的一条**：与其反复猜测再烧录验证，不如先花十分钟**加一段能一次性区分两种可能性的诊断代码**。为了定位 TX dead，前后烧了四五次；如果第一次就写短接自发自收，一次就能定性。

---

## H1. Bootloader 与 App 各写一份分区契约（本次修复 —— 架构级）

**现象**：没有现象。这正是它危险的地方 —— 它不出错，它在等一个时机出错。

**根因**：分区地址常量和 `BootFlag_t` 结构体被抄了两份：

```
bootloader/Inc/boot_cfg.h      → BOOT_ADDR / APP_ADDR / BANK_A_ADDR ... + BootFlag_t
miniweather/BSP/Inc/flash_if.h → FLASH_IF_BOOT_ADDR / FLASH_IF_APP_ADDR ... + BootFlag_t
```

两边靠注释互相警告：

```c
/* ⚠️⚠️ 最高优先级的一条规则：
 *     本文件里的【分区地址】和【BootFlag_t 结构体】必须与 App 工程
 *     BSP/Inc/flash_if.h 里的定义【逐字节一致】。
 *     改任何一处，两边都要改。 */
```

**错在哪**：**用注释约束编译器该管的事**。两个独立工程、两个编译器，
没有任何机制保证这两份定义一致。人一定会忘。

后果链条是这样的：
- 某天把 `active_bank` 和 `upgrade_flag` 换个位置，只改了 App 侧
- OTA 全程正常：`Comm_HandleFirmwareBlock` 写数据、`FINISH` 校验 CRC 全过
- 上位机收到 `OK`，用户以为升级成功
- 重启 → Bootloader 按**自己的**结构体解释那 24 字节 → `upgrade_flag` 读成了 `active_bank` 的值
- 判定"无升级任务"，直接跳老固件；或者更糟，从错误地址搬运 → **变砖**

而且这类 bug 在实验室几乎不复现（两份一致时完全正常），**只在改过一边之后才爆**，
排查时没人会想到"两份结构体不一样"。

**修法**：抽出单一真源 `shared/flash_layout.h`：

```c
/* 只放契约：地址 + 结构体 + 魔数 + 状态枚举
   纪律：不许放实现，只能 include <stdint.h>（Bootloader 用不了 HAL） */
#define BOOT_ADDR        0x08000000u
#define APP_ADDR         0x08010000u
#define BANK_A_ADDR      0x08060000u
...
typedef struct { ... } BootFlag_t;
```

- `bootloader/Inc/boot_cfg.h`  → 只剩 `#include "flash_layout.h"` + Boot 私有配置
- `BSP/Inc/flash_if.h`         → 删掉全部地址与结构体，改为 `#include "flash_layout.h"`
- `BSP/Inc/comm.h`             → `OTA_BANK_A_ADDR` 直接取 `BANK_A_ADDR`（不再取 `FLASH_IF_*`）
- `BSP/Src/flash_if.c`         → `FLASH_IF_PTR` → `FLASH_PTR8`，`FLASH_IF_FLAG_ADDR` → `FLAG_ADDR_MAIN`

两个工程各加一次 Include Path：`..\..\shared`。
从此**物理上不可能不一致** —— 因为只有一份。

> **一句话教训**：**凡是两个独立编译单元都必须同意的东西，只能存在一份。**
> 靠注释、靠约定、靠"改的时候记得同步"，都是在给未来的自己埋雷。
> 判断标准很简单：如果我改了 A 却忘了改 B，编译器会不会报错？
> 会 → 安全；不会 → 迟早出事。

---

## H2. Bootloader 源文件缺设备头（本次修复）

**现象**：`boot.c` 报一堆 `undeclared`：`'RCC' undeclared`、`'USART1' undeclared`、
`'RCC_AHB1ENR_GPIOAEN' undeclared`……

**根因**：`boot.c` 和 `flash_boot.c` 直接操作寄存器（`RCC->AHB1ENR |= ...`、
`USART1->BRR = ...`、`FLASH->KEYR = ...`），但两个文件都没包含 `stm32f4xx.h`。
只有 `main.c` 包含了它 —— 而 C 是**每个 .c 独立编译**的，
`main.c` 里的包含对 `boot.c` 毫无帮助。

**错在哪**：误以为"工程里某个文件包含了，全工程就有了"。
头文件的作用域是**单个翻译单元**，不是整个工程。

**修法**：两个文件顶部各加一行，并且必须放在其他 include **之前**
（设备头里定义了后面要用的外设基址与类型）：

```c
#include "stm32f4xx.h"        /* ★ 第一个包含 */

#include "boot.h"
#include "boot_cfg.h"
...
```

配套：**Keil Options → C/C++ → Define 里必须有 `STM32F407xx`**，
否则 `stm32f4xx.h` 不知道该引入哪个芯片的头，报 `#error "Please select first the target..."`。

> **一句话教训**：**用到 `RCC->xxx` 就得 `#include "stm32f4xx.h"`，一个 .c 一份，不能蹭别人的。**
> 裸机工程里这条最容易漏，因为 HAL 工程习惯性地通过 `main.h` 间接拿到，
> 写裸机时那个"隐形依赖"突然就没了。

---

## H3. 搬运失败后清掉升级标志 → 一次断电永久变砖（本次修复，P0）

**现象**：OTA 下载全程正常，上位机收到 `OK`。搬运途中拔一次电，
之后每次上电串口都打印 `!! APP invalid @0x08010000, HALT`，再也起不来。

**根因**：`boot.c` 的失败分支把 `upgrade_flag` 清成了 `NONE`：

```c
} else {
    flag.upgrade_flag = UPGRADE_NONE;      /* ← 致命 */
    if (flag.boot_count < 0xFFu) flag.boot_count++;
    FLASH_WriteBootFlag(&flag);
}
```

**错在哪**：搬运的固有风险窗口是"擦掉 App 区之后、写完新固件之前"。
这个窗口里断电，App 区是残的，**唯一能救的就是下次上电再搬一次**。
而清掉标志等于亲手关掉了这扇门：

```
搬运中途断电（App 区已擦）
  ↓ 重新上电
读标志：upgrade_flag = NONE → "没有升级任务" → 不搬 → 直接跳 App
  ↓
AppVectorValid() 失败 → HALT
  ↓
一次断电 = 永久变砖
```

**这正是 OTA 最该防的场景，旧代码把它做成了必然变砖。**

**修法**：失败要按"重试有没有意义"分成两类：

```c
if (r == 0) {
    flag.upgrade_flag = UPGRADE_NONE;  flag.boot_count = 0;   /* 成功 */
} else if (r == -2) {
    /* 源固件 CRC 错：数据本身就是坏的，重试一百次也是错的 → 放弃，保住老固件 */
    flag.upgrade_flag = UPGRADE_NONE;  flag.boot_count = 0;
} else {
    /* -3/-4/-5 擦写失败：多半是掉电 → 必须【保留】PENDING，下电重试 */
    if (flag.boot_count < 0xFFu) flag.boot_count++;
}
FLASH_WriteBootFlag(&flag);
```

配套加了**强制重装键**（上电 800ms 内按住 KEY0/PE4）：
重试次数用尽之后，人还能手动再装一次，不用插 ST-Link。

> **一句话教训**：**写失败处理时，第一件事是问"重试有没有意义"，
> 而不是无脑复位状态。** 把"可恢复故障"当成"永久故障"处理，
> 等于把一个偶发掉电变成永久变砖。
> 判断方法：这个错误下次再来一遍，有可能自己好吗？
> 有可能 → 保留现场等重试；完全不可能 → 才放弃。

---

## H4. App 工程 Keil 缺 `shared` 头文件路径（2026-09-23 用命令行编译抓到，P0 阻塞）

**现象**：打开 App 工程点 Build，**直接 5 个文件同时报错**：

```
..\BSP\Inc\flash_if.h(45): error: #5: cannot open source input file
"flash_layout.h": No such file or directory
```

**错在哪**：第 H1 节做了架构整改 —— 把分区契约抽到 `shared/flash_layout.h`，
`BSP/Inc/flash_if.h` 改成 `#include "flash_layout.h"`。
Bootloader 工程的 IncludePath **加了** `..\..\shared`，
但 **App 工程漏了**（只有 `..\BSP\Inc`，没有 shared）。

也就是说：**代码改对了，工程配置没跟上，打开 Keil 第一下就是红的。**

**修法**（uvprojx 里 2 处 IncludePath 都补上）：
```
Options for Target → C/C++ → Include Paths 末尾追加： ..\..\shared
```
（已用脚本直接写进 `miniweather.uvprojx`，备份在同目录 `.bak`）

**验证**（Keil 命令行编译，比点 GUI 更靠谱）：
```bash
cd "D:/mini weather/miniweather/MDK-ARM"
"D:/Keil5/UV4/UV4.exe" -j0 -b miniweather.uvprojx -o keil_build.log
```
结果：**0 Error**，Program Size: Code=95760（95KB，远小于 320KB App 区）。
Bootloader 工程同样验证：**0 Error 0 Warning**，Code=6484（6.3KB，远小于 32KB）。

> **一句话教训**：**改"代码依赖"的时候，别忘了改"工程配置"。**
> 编译器只认 IncludePath，不认你的目录结构有多合理。
> 这类错误的特点是"一次性炸一片"（5 个文件同时报同一个错），
> 看到这个特征就应该想到是路径配置，而不是 5 个文件都写错了。

**顺带**：App 工程 25 个 warning 里 15 个来自 `BSP/Src/lcd_test.c`
（文件存成 UTF-8，ARMCC 按本地编码解析 → 中文变乱码）。
`lcd_test.c` 是屏幕自检用的，**正式交付时可在 Keil 里 Remove 掉**，
警告立刻清零，还能省 Flash。

---

## H5. 把 `.hex` 当固件直接发送（2026-09-24 发现并修复，P0）

**现象**：上位机选中 Keil 产出的 `miniweather.hex`，信息栏显示：

```
大小 282713 字节 (276.1 KB)
包数 2209（每包 128 字节）
CRC32 0xD0B304EB
```

一眼看着"没报错"，但**三处全是错的**。

**错在哪**：`.hex` 是 **Intel HEX 格式的 ASCII 文本**，不是能直接烧的裸二进制。
每行长这样：

```
: 10 0000 00 90D300205D0201088B7701088D660108 FE
│ │  │    │ │                              └ 校验和(1B)
│ │  │    │ └ 16 字节数据（用 32 个 ASCII 字符表示）
│ │  │    └ 记录类型：00=数据 01=结束 04=地址高位
│ │  └ 记录内偏移(2B)
│ └ 本行数据长度(1B) = 0x10 = 16
└ ':' 行起始符
```

16 字节数据要写 43 个字符 → **hex 天然比 bin 大 2.8 倍**。

把 hex 当裸二进制发下去，**同时错三处**：

| # | 后果 |
|---|---|
| ① | 发的是 ASCII 文本、不是机器码 → 下位机写进 Flash 的是垃圾 |
| ② | 282713 字节 > Bank 上限 262144 → 下位机直接回 `ERR_RANGE`，连 START 都过不了 |
| ③ | CRC32 算的是文本的（`0xD0B304EB`）→ 和真实固件的 CRC32 必然不符，就算塞进去也 `ERR_CRC32` |

**关键数字对照**（同一份固件，用 `fromelf` 生成 bin 后实测）：

| 文件 | 大小 | CRC32 |
|---|---|---|
| `miniweather.hex`（文本，**不能用**） | 282713 字节 | `0xD0B304EB` ❌ |
| `App.bin`（裸镜像，**要用这个**） | **100496 字节（98.1 KB）** | `0xDAB73A11` ✅ |

**修法（两层，都做了）**：

1. **上位机加 Intel HEX 解析**（`host/protocol.cpp` 的 `Proto::loadFirmware()` /
   `parseIntelHex()`）：按**内容**判断（首字符是不是 `':'`）而不是看后缀，
   `.bin` / `.hex` 两种都能吃；解析时校验每行校验和、记录长度、地址连续性，
   还能从 hex 里读出**真实链接基址**并和 `OTA_APP_BASE(0x08010000)` 比对告警。
2. **生成 bin 的正规姿势**（Keil → Options → User → After Build → Run #1）：
   ```
   D:\Keil5\ARM\ARMCC\bin\fromelf.exe --bin -o ..\..\host\App.bin .\miniweather\miniweather.axf
   ```

**回归测试**（`host/selftest_hex.cpp`，11 项全过）——最硬的一条证据：
把 `miniweather.hex` 交给解析器，结果与 `fromelf` 产出的 `App.bin`
**长度相同、逐字节完全一致、CRC32 都是 `0xDAB73A11`**。
同时验证四种坏输入（校验和错 / 长度不符 / 非 HEX 文本 / 文件不存在）都被**明确拒绝**，
而不是"静默产出垃圾"。

> **一句话教训**：**"能不能读"和"读得对不对"是两件事。**
> 用 `readAll()` 什么文件都能读进来，但读进来的是不是你想要的字节，
> 才是决定成败的地方。看到"大小/CRC 和预期差一截但不报错"，
> 第一反应应该是**格式不对**（文本 vs 二进制、压缩 vs 未压缩、带容器 vs 裸数据），
> 而不是"再试一次"。
>
> 附带一条工程习惯：**别靠文件后缀猜格式，要读内容特征**。
> 后缀可以随便改（把 bin 改名成 hex 仍然能正常工作，
> 就是因为解析器看的是首字符 `':'`）。

---

## H6. OTA 卡在「第 N 块失败 ×3 → 中止升级」err=0x06（2026-09-24 现场事故，P0）

### 现场现象

OTA 传到 `193/786` 之后一切正常，突然在**第 470 块**连着三次报错：

```
[01:33:48.935]   第 470 块失败(第 3 次重试)，err=0x06
[01:33:48.935] ! 第 470 块最终失败，中止升级
[01:33:48.935] ========== OTA 失败 ==========
```

`err=0x06` = `ERR_RANGE`（偏移越界 / 大小超限）。

### 先定位：0x06 只可能从两条语句出来

下位机 `comm.c` 的 `HandleUpdateBlock()` 里只有两个地方回 `ERR_RANGE`：

| 行 | 条件 | 含义 |
|---|---|---|
| ① | `off != o->recv_size` | **偏移错位**：上位机报的偏移 ≠ 下位机实际收到的字节数 |
| ② | `recv_size + data_len > total_size` | **超总长**：收过头了 |

第 470 块时 `off = 470 × 128 = 60160`，`total_size = 100496`，
`60160 + 128 = 60288 << 100496`，② 根本不可能触发。
**所以必然是 ① —— 两边对"收到哪了"的认知不一致了。**

### 根因：ACK 丢了，而不是块丢了

停等协议（`发一块 → 等 ACK → 发下一块`）里最容易发生的事故
不是"块丢了"，而是**"块写进去了、ACK 在半路上没了"**：

```
上位机 ── 第 N 块 ──▶ 下位机
                      下位机：写 Flash 成功，recv_size += 128
上位机 ◀─ ACK ┄┄┄┄┄  下位机   ★ 这一个 ACK 没了

上位机等 1.5s 超时 → 重发第 N 块（off 还是老值 60160）
下位机：off(60160) != recv_size(60288) → 回 ERR_RANGE(0x06)
上位机再重发 2 次 → 永远是 0x06（recv_size 只会往前走，不可能退回来）
→ "第 N 块最终失败，中止升级" ✗
```

**老逻辑的致命点**：`off != recv_size` 就一律报错，
把"重发"当成了"错误"。而重发恰恰是协议**设计用来容错**的动作 ——
结果容错动作本身被判为非法，一个字节的 ACK 丢失 = 整次 OTA 报废，
重来一次还会再犯（所以它不是"偶发"，是**必现的结构性缺陷**）。

### 修法一：下位机改【幂等 + 带进度回传】（`BSP/Src/comm.c`）

把三种情形分开处理，而且**所有应答都带 4 字节 `recv_size`**：

| 情形 | 判定 | 处理 |
|---|---|---|
| ① 正常块 | `off == recv_size` | 写 Flash、累 CRC32、回 `OK + recv_size` |
| ② 重复块（ACK 丢了） | `off + len <= recv_size` | **不重写 Flash**，直接回 `OK + recv_size`（幂等） |
| ③ 真错位 | 其余（`off > recv_size` / 部分重叠） | 回 `ERR_RANGE + recv_size`，让上位机按真实进度续发 |

为什么 ② 绝对不能重写 Flash：重复写要再走一遍解锁/编程流程，
既拖慢，又可能把已写好的数据搞坏（跨半字/字对齐时尤其危险）。

配套新增统计 `Comm_t.ota_dup` / `Comm_t.ota_resync`，事后能从
调试器里直接看出"发生过几次重发、几次重对齐"。

### 修法二：上位机认 ERR_RANGE 里带的进度（`host/mainwindow.cpp`）

老代码是 `for (int i = 0; i < blocks; i++)`，块号只增不减，
所以永远无法回退。改成 `while (i < blocks)` + 重对齐：

```cpp
if (a.ok && a.err == ERR_RANGE && a.data.size() >= 4) {
    quint32 real = Proto::getU32BE(a.data, 0);   // 下位机真实进度
    int     j    = int(real / OTA_BLOCK_DATA);
    if (j != i) { jumpTo = j; break; }           // 按它的进度续发
}
```

并加 `resyncLeft = 32` 上限防止异常时死循环。

### 修法三：加 OTA 空闲超时（顺手救了一个"板子锁死"）

事故后下位机会**永远停在 `OTA_RECEIVING`**（没人通知它上位机已经放弃了），
而 `HandleUpdateStart` 只在 `IDLE / DONE / FAIL` 时才接受新的 Start ——
于是第二次升级直接吃 `ERR_BUSY(0x04)`，表现成**"板子锁死，只能按复位"**。

新增 `OTA_IDLE_TIMEOUT_MS = 10000`：`READY / RECEIVING` 状态下
超过 10 秒没有新块到达就自己退回 `FAIL`，下一次 START 能重新开始。

### 验证

- Keil 命令行编译 App 工程：`0 Error(s)`（`comm.c` 仅剩 1 条中文编码 warning）
- Qt MinGW 重新编译上位机：通过，产物已部署
- 重新生成 `host/App.bin`：**100680 字节 / CRC32 = `0x4EC1A8FA` / 787 包**
  （入口 `Reset=0x0801025D` → 基址确认为 `0x08010000` ✅）

### 一句话教训

> **停等协议里，"重发"是容错手段，不是错误。**
> 任何"收到重复请求就报错"的设计，都会把一次可恢复的单包丢失
> 放大成不可逆的整体失败。正确姿势是：
> **服务端幂等 + 每次应答回传真实进度 + 客户端按进度续发。**
>
> 顺带两条通用习惯：
> ① 错误应答**不要只回一个错误码**，把能让对端恢复的上下文（这里是进度）带上；
> ② 任何"等待对端"的状态机都要有**空闲超时**，否则对端一崩你就永久卡住。

---

# 九、ESP8266 / WiFi / TCP / AI 层（2026-09-27~28 阶段5 踩坑全集）

> 这一阶段是本项目**排查时间最长、假象最多**的一段。核心教训一句话：
> **"看起来通了"和"真的通了"之间隔着三层假象，唯一能穿透它们的只有"把原始字节打出来看内容"。**

## I1. 【P0·硬件】P2 跳线插错排 → 模块在板上永远 0 字节

**现象**：模块拔下来用 USB-TTL 发 `AT` 回 `OK`；插回板子一个字节都收不到。
排查两天，中途误判过"供电不足""GPIO_0 进下载模式""虚焊"。

**根因**：探索者板的 **P2 跳线**（丝印 RS232/BTCOM&GPS）决定 USART3 通向哪一侧：

| | 左列 | 右列 |
|---|---|---|
| 上排 | 2 = U3_RX | 1 = U3_TX ← **RS232/COM3 侧，不能插** |
| 中排 | **4 = PB10**（TX） | **3 = PB11**（RX） |
| 下排 | **6 = GBC_RX** | **5 = GBC_TX** |

插到上排时 PB10/PB11 被接到板载 **SP3232**（RS232 电平转换芯片），其 ROUT 输出级
**强拉** PB11 为高；而模块 TXD 是**弱驱动**（串 MBR0520 肖特基 + 5.6K 上拉），顶不动 → 0 字节。

**正确插法**：两根跳线帽**纵向**插 —— 左列 4↔6（PB10↔GBC_RX）、右列 3↔5（PB11↔GBC_TX）。
插成横向（4↔3 / 6↔5）= 只把 PB10 与 PB11 短接、GBC 侧悬空。

**为什么这么久才定位**：阶段2~4 的协议联调是**杜邦线直插 PB10/PB11 排针**，
**绕过 P2** → P2 这一段从来没被真正验证过。

## I2. 【P0·方法论】相邻引脚串扰 → 所有"回环 4/4"全是假象

**现象**：模块拔掉、板上一根线都没连，loopback 测试**稳定 4/4**。

**根因**：PB10(TX) 与 PB11(RX) 在 LQFP144 上是**相邻引脚**。模块拔掉后 PB11 悬空成高阻，
两脚间几 pF 寄生电容把 TX 跳变直接耦合进 RX 输入 → USART 看到"假起始位"
→ 收到**它自己刚发出去的字节**。

**指纹**：回显内容会失真（`"AT\r\n"` 回来变成 `"AT\n"`，`\r` 丢了）。真回环不可能失真。

**后果（重大）**：此前所有"短接 GBC 得 4/4、短接 U17 3-4 脚得 4/4 → 板级链路通"
的结论**全部作废**，那些测试等于什么都没证明。

> ### ⛔ 方法论红线（永久）
> **自发自收的回环测试，只要 RX 脚处于高阻（对端没在驱动），结论一律不可信。**
> 要让它可信，必须**探针期间把 RX 脚强制设成内部下拉**（40kΩ 下拉把耦合电平
> 压到 V_IL 以下；真短接 <1Ω 与真实模块信号 5.6k 上拉不受影响）。

## I3. 【方法论】ROM 日志抓取时机错了 = 抓了个寂寞

ESP8266 的 74880 ROM 日志**只在复位后约 200ms 内吐一次**。放在 10 次 AT 重试之后去抓，
模块早已说完，必然 `heard 0 byte(s)`，**证明不了任何事**。必须紧跟 `hw_reset()` 之后抓。

衍生出一个更好用的指标：**直接读 GPIO IDR 数边沿**（AF 模式下 IDR 仍反映真实电平）。
UART 只会说"收到/没收到"，波特率错时"没收到"和"线断了"打印出来一模一样，
**数边沿才能区分**：N>50 = 线上有真实活动，是我们波特率/解析不对；N≈0 = 线是死的。

## I4. 【误判·已撤回】GPIO_0 被拉低进下载模式

曾假设"模块 GPIO_0 吊在 PC0 上，被板载 QMA6100P 的 3D_INT 拉低 → 上电进 UART 下载模式"，
后经用户澄清 KEY/LED 两脚悬空而**撤回**。

留下的教训：原理图里 **PC0 是双负载网络**（U17 第1脚 + QMA6100P 的 3D_INT）。
**不要用多负载网络的电平去反推某一个器件的状态** —— 测出来的方波可能来自任何一个。

## I5. 【配置】Windows 移动热点连不上：`+CWJAP:3`

`+CWJAP:<code>`：1=超时 2=密码错 **3=找不到目标 AP** 4=连接失败。
拿到 **3** 说明**扫都扫不到**，通常是**频段 5GHz**（ESP8266 只支持 2.4GHz），
不是密码问题（密码错是 2）。改法：设置 → 移动热点 → 网络频段改 **2.4GHz** → 重启热点。
另：ESP8266 不支持 WPA3，只支持 WPA/WPA2。

## I6. 【环境】防火墙拦掉了 TCP 8888

Windows 移动热点算**公用**网络，入站默认全拦。必须**管理员权限**执行：

```powershell
New-NetFirewallRule -DisplayName "MiniWeather TCP 8888" -Direction Inbound -Protocol TCP -LocalPort 8888 -Action Allow -Profile Any
```

⚠️ 顺序：**先起服务端，再复位板子**。服务端没监听与防火墙拦，现象完全一致（都是连不上）。

## I7. 【假象】透传模式下再发 AT → 全部 FAILED

进透传后模块**不再解析 AT**，"AT\r\n" 被当**数据**塞进 TCP，模块只回 `CIPSEND` 后的
`>` 提示符（hex 末尾 `3E`）。所以 6 条 AT 全部 `rc=1`、hex 一律 `0D 0A 4F 4B 0D 0A 0D 0A 3E`。

→ `ESP_PROOF_LINK` 在透传方案下**必须为 0**，否则那 6 条垃圾串会灌进服务器污染应用数据。

## I8. 【P0·软件】AI 文本旁路吃掉长帧分片 → WiFi OTA 每块重试 2~3 次

**现象**：START/心跳正常（状态 2 就绪），固件块每块超时重发 2~3 次才过，
偶尔整块三次全超时 → `OTA 失败`。

**根因**：UART 是**字节流**。为在同一条 TCP 上区分"协议帧"和"AI 建议文本"，加了按首字节分流：

```c
if (!(本段以 AA 55 开头)) { AI_FeedBytes(本段); return; }   /* ← 出事的判据 */
```

140 字节的固件块在 WiFi 下几乎必然被切成多段（ESP8266 按 TCP 分片吐 +
TaskComm 5ms 才取一次）。第 1 段 `AA 55` 开头 → 喂协议层；
**第 2 段开头是数据字节 → 被判成 AI 文本旁路丢弃** → 协议层永远收不满 → 不应答 → 超时重发。

串口直连时 PC 一口气发完、常常一次取全，所以**只在 WiFi 下暴露**。

**修法**：旁路加前提 —— **只有解析器在等帧头时才允许旁路**：

```c
if ((g_proto.state == P_ST_HEAD_HI) && !(n >= 2 && g_rx_tmp[0]==0xAA && g_rx_tmp[1]==0x55)) {
    AI_FeedBytes(g_rx_tmp, n); Proto_Tick(&g_proto, COMM_NOW_MS()); return;
}
```

> ### ⛔ 方法论红线（永久）
> **给字节流加"按首字节分流"的逻辑时，必须考虑分片。**
> 首字节判据只对"整帧一次到达"成立；流式链路上必须**结合解析器状态**（进帧后一律归协议层）。

## I9. 【小】服务端 `ACK:` 回执被当成 AI 建议显示

`ai_tcp_server.py` 对协议帧回 `ACK:<len>`，这行纯 ASCII 下行会被板侧当建议收走，
LCD 显示 `ACK:15` 而不是大模型建议（真建议要等 60 秒才覆盖掉它）。
→ 下行链路验证完就该把 ACK 回执关掉，或在板侧丢弃 `ACK:` 开头的行。

## I10 【P0·软件】OTA 期间 AI 上报抢 USART3 → 应答被背压堵 5.8 秒 → 升级在第 223 块中止

**现象**（2026-09-28 23:19，修完 I8 分片 bug 之后再次 WiFi OTA）：

```
[23:19:48.541] RX ← AA 55 80 04 00 05 00 00 00 6F 80 C0 46   ← 第 222 块 OK（recv=0x6F80）
[23:19:48.542] TX → AA 55 00 04 00 84 00 00 6F 80 ...         ← 发第 223 块
[23:19:50.045]   第 223 块失败(第 1 次重试)，err=0xff          ← 1.5s 超时
[23:19:51.579]   第 223 块失败(第 2 次重试)，err=0xff
[23:19:53.100]   第 223 块失败(第 3 次重试)，err=0xff
[23:19:53.121] ! 第 223 块最终失败，中止升级                    ← 223/787 就挂了
[23:19:54.368] RX ← AA 55 80 04 00 05 00 00 00 70 00 50 4F     ← 应答其实【早就好了】，迟到 5.8s
[23:19:54.611] RX ← ...50 4F AA 55 80 04 ... 00 70 00 50 4F    ← 两条 ACK 粘成一坨同时到
```

**关键判读**：应答里的 `00 70 00`（recv_size = 0x7000 = 28672 = 28544+128）**完全正确** ——
下位机早就收下并写好这一块了，只是应答出不来。所以这不是协议问题、不是 CRC 问题、也不是 I8 的分片问题。

**根因**：`TaskSensor` 每 60 秒调一次 `AI_Report()` → `ESP8266_SendData()` 往 **USART3** 塞 JSON。
而 USART3 此刻正被 OTA 下行固件块高速占用。两条流抢同一个 ESP8266 收发缓冲 → 模块内部背压 →
下位机组好的应答帧在模块里排队。两条 ACK 最后**粘成一坨同时到达**就是背压释放的直接证据。
雪上加霜的是优先级：`TaskSensor(osPriorityHigh) > TaskComm(osPriorityNormal)`，
上报一发生就抢走 CPU，进一步拖慢协议任务。

**修法（两处，都改了）**：
1. **板侧 `freertos.c`**：`ERASING / READY / RECEIVING` 期间**不上报**，
   只在 `IDLE / DONE / FAIL` 按原节奏发。OTA 结束后自动恢复。
2. **上位机 `mainwindow.cpp`**：块超时 1500ms → **3000ms**，重试 3 → **4** 次，
   重试间隔 20ms → **150ms**（重发太急会把模块缓冲灌满，越急越慢）。

> ### ⛔ 方法论红线（永久）
> **一条 USART 上跑了几种不同性质的数据流时，必须先问"它们在抢什么"。**
> OTA 是"大吞吐 + 对延迟敏感"，AI 上报是"周期性 + 可延后"——
> 后者必须让路，而不是靠"加重试"去硬扛。
> 另：**高优先级任务里不要做阻塞式外设发送**，它会把低优先级任务的实时性一起拖垮。

## 这一阶段的通用经验

| # | 经验 |
|---|---|
| 1 | **"能收到字节" ≠ "读得对"**；判"对了"的硬标准是**两条独立路径产物一致**，或**对端报出的内容 MCU 自己造不出来**（如 `AT+GMR` 的 179 字节版本串） |
| 2 | 排查要**分层**（串口层→WiFi层→TCP层→应用层），每层先证伪再往下 |
| 3 | 自发自收的测试必须先把 RX 从高阻变成有确定性电平，否则结论无效 |
| 4 | 用户在现场看到的事实优先于推断；推断错了要明确撤回并说明 |
| 5 | 流式协议的分流逻辑，**永远要假设一帧会分多次到达** |
