# Bootloader 工程说明

## 一、为什么它是独立工程（不放在 miniweather/BSP 里）

| | Bootloader | App |
|---|---|---|
| 链接地址 | `0x08000000`（S0+S1，32KB） | `0x08010000`（S4~S6，320KB） |
| 入口 | 有自己的 `main()` | 有自己的 `main()` |
| 依赖 | 裸机，只用 CMSIS 设备头 | HAL + FreeRTOS + lwIP |
| 向量表 | `SCB->VTOR = 0x08000000`（默认） | `SCB->VTOR = 0x08010000` |

两个 `main()` 不可能共存于同一个 Keil 工程；两个不同的 IROM 起始地址也不可能
共用一个链接配置。**所以物理上必须分成两个工程。**

`miniweather/BSP/` 里放的是"App 用的板级驱动"，Bootloader 不是驱动，是另一段固件。

---

## 二、两个工程的共享部分：只有一个文件

```
D:\mini weather\shared\flash_layout.h     ← Boot 与 App 的唯一契约
```

里面只有：分区地址常量、`BootFlag_t` 结构体、魔数、`UPGRADE_*` 状态、XIP 读指针宏。
**不依赖 HAL / FreeRTOS / main.h**，因为 Bootloader 用不了它们。

两个工程各自加一次 Include Path 就能共用，从此不可能"改一边忘另一边"。

> 改这个文件 = 同时改两个固件，必须一起重新编译烧录。

---

## 三、文件清单

```
bootloader/
├─ Inc/
│  ├─ boot_cfg.h      Boot 侧配置入口（转出 shared/flash_layout.h + 串口/按键配置）
│  ├─ boot.h          主流程声明
│  ├─ flash_boot.h    寄存器版 Flash 驱动声明
│  └─ crc32.h         CRC32
├─ Src/
│  ├─ main.c          入口：Boot_LogInit() → Boot_Run() → 死循环等救援
│  ├─ boot.c          判标志 → 搬运 → 校验 → 跳转 + 极简串口日志 + 强制重装键
│  ├─ flash_boot.c    直接操作 FLASH 寄存器的擦/写/读 + 启动标志乒乓
│  ├─ crc32.c         CRC32 流式计算（与 App 侧算法完全一致）
│  └─ system_stm32f4xx.c  从 App 工程拷来的时钟初始化（提供 SystemCoreClock）
├─ MDK-ARM/
│  ├─ bootloader.uvprojx   Keil 工程（已配好：器件 / 文件 / 包含路径 / IROM 32KB）
│  └─ startup_stm32f407xx.s
└─ README.md          本文件
```

Keil 工程**已经生成好了**，直接双击 `MDK-ARM/bootloader.uvprojx` 打开即可，
不用手动建工程、不用手动加文件。

App 侧对应的是 `miniweather/BSP/Src/flash_if.c`（基于 HAL 的同功能实现）。
**两边实现各自独立，契约共享** —— 这才是分层的意思。

---

## 四、Keil 工程（已经配好，打开即用）

工程文件：`bootloader\MDK-ARM\bootloader.uvprojx`
（它是从 App 工程改造来的，所以器件、编译器版本、调试器设置都跟你现有工程一致）

已配置好的项：

| 配置项 | 值 | 在哪看 |
|---|---|---|
| 器件 | STM32F407ZGTx | Options → Device |
| 编译器 | ARMCC V5（与 App 工程同） | Options → Target → Code Generation |
| **Define** | `STM32F407xx`（**没有** USE_HAL_DRIVER） | Options → C/C++ |
| **Include Paths** | `..\Inc`、`..\Src`、`..\..\shared`、两条 CMSIS 路径 | Options → C/C++ |
| **IROM1** | `0x08000000`，大小 `0x8000`（32KB） | Options → Target |
| **IRAM1** | `0x20000000`，大小 `0x20000`（128KB） | Options → Target |
| 源文件组 | `Bootloader/Src`（5 个 .c）+ `Application/MDK-ARM`（startup.s） | 左侧工程树 |

**唯一要手动确认的**：Options → Debug → 确认是 ST-Link，且
**Utilities → Settings → Flash Download 里勾 "Erase Sectors" 而不是 "Erase Full Chip"**
—— 后者会把 App 区一起擦掉。

> 如果打开时 Keil 提示找不到器件包，说明你 App 工程能编译就一定能编译，
> 检查一下是不是用同一个 uVision 版本打开的。

## 四·补、编译出来有多大

Bootloader 目标 ≤ 32KB（S0+S1）。当前代码量：
`boot.c` + `flash_boot.c` + `crc32.c` + `main.c` + `system_stm32f4xx.c` ≈ 几百行 C，
ARMCC -O0 下大概 4~8KB，非常宽裕。
**如果编译报 `L6406E: No space in execution regions`，说明超了 32KB**，
要么开 -O2，要么把日志里的 `vsnprintf`（会拉进一坨库函数）换成手写的简易格式化。

---

## 五、第一次烧录的顺序（重要）

1. 先单独烧 **Bootloader**（它只占 S0+S1）
2. 再把 App 工程改成配合 Bootloader 的模式：
   - `miniweather/Core/Inc/main.h`：`#define APP_WITH_BOOTLOADER 1`
   - Keil → Target → IROM1：`Start = 0x8010000`，`Size = 0x50000`
   - Keil → C/C++ → Include Paths 加：`..\..\shared`
3. 重新编译 App，烧录到 `0x08010000`
4. 上电：Boot 先跑 → 读标志 → 无升级任务 → 跳 App

> 只要 `APP_WITH_BOOTLOADER = 0`（当前默认），App 就是独立固件，
> 可以 ST-Link 直烧 `0x08000000` 正常跑，完全不受 Bootloader 影响。

---

## 六、启动日志怎么看

上电后 PC 串口助手（USART1，PA9/PA10，115200 8N1）会看到：

```
==== MiniWeather Bootloader ====
boot @0x08000000, app @0x08010000
flag: none/invalid -> run app as-is
jump to app...
```

有升级任务时：

```
flag: ver=0x00010000 bank=0 upg=1 bootcnt=0 size=123456
upgrade pending: bank 0 -> app
  crc calc ... 0x12345678 (want 0x12345678)
  erase app ... ok
  copy ... ok
  verify 0x12345678
upgrade DONE
jump to app...
```

搬运失败（比如中途断电）时：

```
  copy ... FAIL@0x00004000
!! copy fail(-4) -> KEEP pending, retry next boot
!! (boot_count=1/3)
```

> **注意这里的关键设计**：失败后**保留**升级标志，下次上电自动重试（最多 3 次）。
> 只有"Bank 里的固件 CRC 本身就不对"才彻底放弃 —— 因为那种情况重试也没用。

App 区损坏时会 HALT（不复位重启，方便接串口排查）：

```
!! APP invalid @0x08010000, HALT
!! reflash app via SWD
```

## 七、救砖：强制重装

**场景**：搬运途中反复断电，3 次都失败 → Boot 放弃 → App 区是残的 → 变砖。

**操作**：上电后 **800ms 内按住 KEY0（PE4）**，Boot 会无视失败计数，
强制从 Bank 再装一次：

```
[KEY ] KEY0 held -> FORCE reinstall
upgrade pending: bank 0 -> app
...
```

前提是 Bank 里的固件本身是完好的（能过 CRC）。
