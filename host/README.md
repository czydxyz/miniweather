# mini-weather 上位机（Qt 5.12.9）

用 Qt Widgets 写的 PC 端工具，干两件事：

1. **监控**：查版本、心跳、读传感器（温/湿/压/CO2/TVOC）、控制舵机/继电器/蜂鸣器
2. **OTA**：把 `App.bin` 通过串口或 WiFi 分块发给下位机，CRC32 对账后切 Bank

## 文件

| 文件 | 作用 |
|---|---|
| `miniweather_host.pro` | qmake 工程文件（必须 `QT += serialport network`） |
| `main.cpp` | 程序入口，创建 QApplication 并进入事件循环 |
| `protocol.h / .cpp` | 协议层：CRC16/CRC32、打包、接收状态机、**固件加载（.bin/.hex 自动识别）**（**与下位机一一对应**） |
| `mainwindow.h / .cpp` | 界面 + 串口/TCP 通道 + OTA 流程 |
| `selftest_crc.cpp` | 协议自检程序（不加入工程，单独编译，见下） |
| `selftest_hex.cpp` | 固件加载回归测试：hex 解析结果必须与 fromelf 的 bin 逐字节一致 |
| `App.bin` | 已经用 fromelf 生成好的固件镜像（100496 字节，CRC32 `0xDAB73A11`） |
| `bin/miniweather_host.exe` | ★ **已编译好的成品**，含全部 DLL，**双击即可运行** |

## 🚀 最快上手

直接双击 `bin\miniweather_host.exe`。
（Qt 5.12.9 MinGW 64-bit 编译，Qt 运行库已在同目录，不需要装 Qt。）

想改代码再用 Qt Creator 打开 `.pro` 重新构建。

## 协议自检（可选，答辩能演示）

验证上位机的 CRC 算法与下位机**逐字节一致**：

在 Git Bash 里跑（Qt 装在 `D:\Qt\Qt5.12.9`）：

```bash
export PATH="/d/Qt/Qt5.12.9/5.12.9/mingw73_64/bin:/d/Qt/Qt5.12.9/Tools/mingw730_64/bin:$PATH"
QTD=/d/Qt/Qt5.12.9/5.12.9/mingw73_64
cd /d/mw_build          # 构建目录，里面有 protocol.o 和 moc_protocol.o
g++ -std=gnu++11 -I"D:/mini weather/host" -I$QTD/include -I$QTD/include/QtCore \
    "D:/mini weather/host/selftest_crc.cpp" release/protocol.o release/moc_protocol.o \
    -L$QTD/lib -lQt5Core -lole32 -lws2_32 -lwinmm -luuid -ladvapi32 -lshell32 -luser32 \
    -o test_crc.exe
./test_crc.exe
```

实测输出（7 项全过）：

```
CRC16("123456789")       = 0x4B37       OK
CRC32("123456789")       = 0xCBF43926   OK
buildFrame(0x0001)       = AA 55 00 01 00 00 E4 51   ← 与《第9节》手算联调帧一致
往返测试 / 拆包测试 / 干扰测试 / CRC 错误检测       全部 OK
```

## 编译运行（Qt Creator）

1. 打开 Qt Creator → 文件 → 打开文件或项目 → 选 `miniweather_host.pro`
2. 套件（Kit）选 **Desktop Qt 5.12.9 MinGW 64-bit**（不要选 MSVC，除非你装了对应版本）
3. 左下角选 Debug 或 Release → 点锤子（构建）→ 点三角（运行）

⚠️ 本项目路径 `D:\mini weather\host` **含空格**，个别机器上 qmake 会出奇怪的错。
若构建报错提到路径，把整个 `host` 文件夹复制到 `D:\mw_host` 再打开即可。

## 命令行编译（已实测可用）

```bash
mkdir D:\mw_build && cd D:\mw_build
set PATH=D:\Qt\Qt5.12.9\5.12.9\mingw73_64\bin;D:\Qt\Qt5.12.9\Tools\mingw730_64\bin;%PATH%
qmake.exe "D:\mini weather\host\miniweather_host.pro" -spec win32-g++
mingw32-make.exe -j4
release\miniweather_host.exe
```

## 两种连接方式

| 模式 | 怎么用 | 下位机侧要求 |
|---|---|---|
| **串口** | USB-TTL 接 **PB10(TX)/PB11(RX)** + GND，115200 8N1 | **必须拔掉 ESP8266**，否则两个 TX 打架 |
| **TCP 服务器** | 上位机填端口（默认 8888）点"打开"，等下位机连进来 | ESP8266 已配好 `TCP_SERVER_IP/PORT`，进透传 |

协议跑在 USART3 上，所以两种模式是"同一根线、两种延长方式"。

## ⚠️ 固件文件用 `.bin`，别用 `.hex`

Keil 默认产物是 `.hex` —— 那是 **Intel HEX 格式的 ASCII 文本**，不是能直接发的裸二进制。
本工程实测两者差 2.8 倍：

| 文件 | 大小 | CRC32 | 能不能用 |
|---|---|---|---|
| `miniweather.hex` | 282713 字节（276 KB） | `0xD0B304EB` | ❌ 文本，超 Bank 上限 |
| `App.bin`（fromelf 产出） | **100496 字节（98 KB）** | **`0xDAB73A11`** | ✅ 要发的是这个 |

**现在的版本两种都支持**：加载文件时按**内容**判断（首字符是不是 `':'`）而不是看后缀，
`.hex` 会被自动解析成裸镜像，还会校验里面的链接基址是否等于 `0x08010000`。
但**最省事、最不容易出错的做法还是直接用 `.bin`**。

生成 bin（Keil → Options for Target → User → After Build/Rebuild → Run #1 打勾）：
```
D:\Keil5\ARM\ARMCC\bin\fromelf.exe --bin -o ..\..\host\App.bin .\miniweather\miniweather.axf
```
（`..\..\host\App.bin` 是相对 `MDK-ARM` 目录的路径，所以回到 `D:\mini weather\host\`）

## OTA 操作步骤

1. 选固件：优先 `host\App.bin`；选 `.hex` 也能用（会自动解析）
   - 信息栏会显示 `大小 / 包数 / CRC32`，`.hex` 还多一行 `起始地址 0x08010000`
   - 若显示 `⚠ 超过 Bank 容量 262144 字节` 或 `⚠ 基址应为 0x08010000`，**别硬发**
2. 目标 Bank 选 0（A 区 `0x08060000`）
3. 点"开始升级" → 自动走完 Start / 等擦除 / 2048 块 / Finish / 切 Bank
4. 看到"升级完成"弹窗后 **手动复位开发板**，Bootloader 会把 Bank 搬到 App 区并跳转

⚠️ 搬运期间（1~3 秒）**不要断电**。Bootloader 有 3 次失败自动回滚保护。

## 固件加载回归测试（可选，答辩能演示）

验证 hex 解析器"读得对"（不只是"读得到"）：

```bash
cd /d/mw_build
export PATH="/d/Qt/Qt5.12.9/5.12.9/mingw73_64/bin:/d/Qt/Qt5.12.9/Tools/mingw730_64/bin:$PATH"
g++ -std=gnu++11 -I"D:/mini weather/host" -I/d/Qt/Qt5.12.9/5.12.9/mingw73_64/include \
    -I/d/Qt/Qt5.12.9/5.12.9/mingw73_64/include/QtCore \
    "D:/mini weather/host/selftest_hex.cpp" release/protocol.o release/moc_protocol.o \
    -L/d/Qt/Qt5.12.9/5.12.9/mingw73_64/lib -lQt5Core -lole32 -lws2_32 -lwinmm -luuid \
    -ladvapi32 -lshell32 -luser32 -o test_hex.exe
./test_hex.exe
```

实测 11 项全过，核心一行是 `[PASS] 内容与 App.bin 逐字节完全一致`。
