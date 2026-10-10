# MiniWeather — 微气候监测 + 双 Bank 防砖 OTA + AI 顾问

基于 **STM32F407ZGT6（正点原子探索者）** 的嵌入式综合项目：多传感器采集 → FreeRTOS 多任务调度 →
自定义串口协议 → ESP8266 WiFi 上云 → Qt 上位机 OTA 升级 + AI 建议下发。

> 一个工程覆盖：**BSP 驱动 / FreeRTOS / 自研协议栈 / Bootloader-IAP / 双 Bank 防砖 OTA /
> Qt 上位机 / 大模型联动** 七块，自研代码约 8000 行。

---

## 硬件

| 类别 | 器件 | 接口 |
|---|---|---|
| 主控 | STM32F407ZGT6（LQFP144，1MB Flash，HSE 8MHz，SYSCLK 168MHz） | — |
| 温湿度 | SHTC3 | I2C1 `0x70` |
| 气压 | BMP280 / BME280 | I2C1 `0x76`（ID `0x58`/`0x60` 自动识别） |
| 空气质量 | SGP30（CO₂eq / TVOC） | I2C1 `0x58` |
| EEPROM | AT24C02（存 SGP30 基线） | I2C1 `0x50` |
| SPI Flash | W25Q128（16MB） | SPI1，CS=PB14 |
| 显示 | ILI9341 2.4" SPI 屏 | SPI2，CS=PB12 / DC=PE7 / BL=PE8 / RST=PA4 |
| WiFi | ATK-MW8266D（ESP8266） | USART3 PB10/PB11，115200 |
| 调试 | CH340 | USART1 PA9/PA10，115200 |
| 执行器 | 舵机（TIM3_CH1=PA6）/ 蜂鸣器（PF8）/ 继电器（PE5） | PWM / GPIO / GPIO |

---

## Flash 分区（`shared/flash_layout.h` 为唯一契约）

| 区 | 起始地址 | 大小 | 用途 |
|---|---|---|---|
| Bootloader | `0x08000000` | 32 KB | 启动判定与固件搬运 |
| 标志区 | `0x08008000` | 32 KB | 升级标志（S2 主 + S3 备，乒乓冗余） |
| App | `0x08010000` | 320 KB | 运行区，固件上限 256 KB |
| Bank A | `0x08060000` | 256 KB | OTA 下载区 |
| Bank B | `0x080A0000` | 256 KB | OTA 下载区 |

Bootloader 与 App 是**两个独立 Keil 工程**，共用 `shared/flash_layout.h` 作为唯一布局来源。

---

## 数据流

```
传感器 ──I2C──▶ TaskSensor ─┐
                            ├─▶ 共享数据（临界区保护）─┬─▶ TaskDisplay ─▶ LCD
执行器 ◀──GPIO/PWM──────────┘                        └─▶ TaskComm ──USART3──▶ ESP8266 ──WiFi──▶ Qt 上位机
                                                                                                    │
                                                                                          AI 建议文本 ▼
                                                                                                   LCD 滚动显示
```

---

## 自研协议

```
AA 55 | CMD(2B BE, bit15=1 为应答) | LEN(2B BE) | DATA(N) | CRC16(2B BE)
整帧 8+N 字节，N ≤ 256
```

- CRC16/IBM：`poly 0x8005`（反射 `0xA001`），`init 0xFFFF`，右移，结果不取反
- CRC 范围 = 命令字..数据末尾（**不含帧头**）；自检 `CRC16("123456789") == 0x4B37`
- 命令：`0x0001` 版本 / `0x0002` 心跳 / `0x0003` 开始升级 / `0x0004` 固件块 /
  `0x0005` 完成 / `0x0006` 切 Bank / `0x0007` 读传感器 / `0x0008` 控制执行器
- 应答数据区第 1 字节为错误码（`0x00` OK / `0x01` 未知命令 / `0x05` Flash 失败 / `0x08` CRC32 不符）

---

## OTA 可靠性设计

| 机制 | 说明 |
|---|---|
| 停等重传 | 每块等应答，超时重发；**重发复用同一个 offset** |
| **块幂等** | `off < recv_size` 视为重复块（ACK 丢失）→ 不重写 Flash，直接回 OK + `recv_size`；只有 `off > recv_size` 才报错 |
| 断点重对齐 | 应答永远携带 4 字节 `recv_size`，上位机据此重新对齐 |
| 流式 CRC32 | 逐块累计，Finish 时整包比对 |
| 先验后擦 | Bootloader 先校验源固件 CRC32，通过后才擦运行区 |
| 双 Bank + 乒乓标志 | 标志区两份互为备份，擦写掉电不丢状态 |
| 失败回滚 | 搬运失败保留 `UPGRADE_PENDING` 并 `boot_count++`，超阈值放弃重试跑现存 App |
| 独立看门狗 | IWDG（LSI / 64 / Reload 2000 ≈ 4.0 s），空闲任务喂狗，擦写前手动补喂 |

---

## 实测数据

| 项目 | 结果 |
|---|---|
| 固件产物 | `App.bin` = 103704 字节，CRC32 `0xB0CB1B5B` |
| App 编译 | Code=99128，RO=4532，RW=532，ZI=53900 |
| Bootloader 编译 | Code=6492 |
| WiFi OTA | 787 块零丢失，CRC32 计算值与预期一致，升级完成标志已清 |
| 看门狗 | IWDG 约 4 s 超时，全程无咬死 |

---

## 编译

### App（Keil MDK-ARM）

```bat
cd miniweather\MDK-ARM
"D:\Keil5\UV4\UV4.exe" -j0 -b miniweather.uvprojx -o keil_build.log
"D:\Keil5\ARM\ARMCC\bin\fromelf.exe" --bin -o ..\..\host\App.bin .\miniweather\miniweather.axf
```

### Bootloader

```bat
cd bootloader\MDK-ARM
"D:\Keil5\UV4\UV4.exe" -j0 -b bootloader.uvprojx -o keil_build.log
```

### Qt 上位机（5.12.9 / mingw73_64）

```bat
set PATH=D:\Qt\Qt5.12.9\5.12.9\mingw73_64\bin;D:\Qt\Qt5.12.9\Tools\mingw730_64\bin;%PATH%
cd host && mkdir build && cd build
qmake ..\miniweather_host.pro "CONFIG+=release"
mingw32-make -f Makefile.Release -j4
```

> 单固件直烧请把 `main.h` 的 `APP_WITH_BOOTLOADER` 置 0；启用 Bootloader 置 1 并同步
> IROM1 为 `0x8010000` / `0x50000`，VTOR 设为 `0x08010000`。

---

## 目录结构

```
.
├─ bootloader/          Bootloader 独立工程（跑 HSI 16MHz，寄存器版 Flash 驱动）
├─ miniweather/         App 工程（Core/ BSP/ Middlewares/ Drivers/ LWIP/）
├─ shared/              flash_layout.h —— 上下位机与 Boot/App 共用的布局契约
├─ host/                Qt 上位机（协议层 + OTA 工具 + 自检程序）
├─ tools/               一次性脚本（注释整理工具）
└─ bug记录与避坑清单.md   30+ 条真实踩坑记录（现象 → 定位 → 根因 → 修法）
```

---

## 文档

本仓库只保留两份文档：`README.md`（本文件）与 `bug记录与避坑清单.md`。

`bug记录与避坑清单.md` 按 **I 类**编号，每条记录包含现象、定位过程、根因与修法，
是本项目区别于其他仓库的主要资产。

---

## 主要坑位（详见 `bug记录与避坑清单.md`）

- P2 跳线必须纵向插 GBC 侧，插 RS232 侧 USART3 收不到数据（SP3232 强拉 PB11）
- ESP8266 只支持 2.4GHz + WPA2；进透传后 AT 不再被解析
- 自发自收回环测试在 RX 高阻时结论无效（相邻引脚几 pF 耦合即可收到自发字节）
- 单条 USART 跑多种数据流要先问“它们在抢什么”——AI 上报曾与 OTA 块抢缓冲致 ACK 迟到 5.8 s
- F4 Flash 擦除前必须 `__HAL_FLASH_CLEAR_FLAG`，且 `Banks` 必须显式填 `FLASH_BANK_1`
- Bootloader 跳转前 `__disable_irq()` → App 必须在自身开头 `__enable_irq()`
- OTA 只发送裸 `.bin`，发 `.hex` 会导致长度超限与 CRC32 不符
