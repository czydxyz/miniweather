# -*- coding: utf-8 -*-
"""给指定文件在开头补一段「维护要点」注释（只写事实与约束，不写原因）。"""
import io, os

H = {}

H['miniweather/BSP/Src/at24c02.c'] = """/* AT24C02 EEPROM 驱动（硬件 I2C1，板载，地址 0x50）
 * [约束] AT24C02_Init() 会把 PB8/PB9 从 AF4_I2C1 改回 GPIO 开漏输出 → 硬件 I2C 断开，
 *        SHTC3/BMP280/SGP30 全部超时。禁止调用，也禁止在别处重配 PB8/PB9。
 * [约束] 页写 8 字节且不跨页：跨页会回卷到本页开头覆盖已有数据，须按页边界分批。
 * [约束] 每页写后必须延时 >=5ms（写周期），不可删。
 */
"""

H['miniweather/BSP/Src/w25q128.c'] = """/* W25Q128 SPI Flash 驱动（SPI1：PB3/SCK PB4/MISO PB5/MOSI，CS=PB14）
 * [约束] 写前必须擦除（最小单位 4KB 扇区）；页写最大 256B 且不跨页，跨页回卷本页开头。
 * [约束] 每条擦/写后必须等 BUSY 清零：扇区擦典型 45ms / 最大 400ms，整片擦最长 200s。
 * [约束] 与 LCD 共用 SPI1 总线，片选必须互斥，空闲一律拉高。
 */
"""

H['miniweather/BSP/Inc/w25q128.h'] = """/* JEDEC ID：0xEF4018=Winbond W25Q128 / 0x684018=Boya BY25Q128（板载实测为此片）
 *          0xC84018=GigaDevice GD25Q128；三家「类型+容量」均为 0x4018，仅厂商字节不同。
 */
"""

H['miniweather/BSP/Src/lcd.c'] = """/* ILI9341 LCD 驱动（SPI2：CS=PB12 DC=PE7 BL=PE8 RST=PA4）
 * [约束] PA4(RST) 未在 CubeMX 的 gpio.c 中配置，本文件自配推挽输出，勿依赖 CubeMX。
 * [约束] 命令/数据由 DC 电平决定，每次传输必须「先切 DC，再拉 CS」，顺序反了会写错。
 * [约束] 坐标 >255，必须拆高/低字节：0x2A 列地址、0x2B 行地址各 4 个参数。
 * [约束] 字模 asc2_1608 为逐列式，需开「宽1 高16」细窗口逐列刷 8 次，不可开 8x16 大窗口。
 * [约束] MADCTL(0x36) = 0x08（0x48 会左右镜像）。
 * [约束] 批量写像素分包上限见 LCD_BURST_MAX，全屏 240*320*2=153600B 一次传会溢出。
 */
"""

H['miniweather/BSP/Src/comm.c'] = """/* 协议落地层（AA55 | CMD | LEN | DATA | CRC16）
 * [约束] 共享数据必须成对使用 Comm_UpdateSensor() / Comm_GetSensorSnapshot()，
 *        禁止任何地方直接访问 Comm_GetCtx()->xxx。
 * [约束] OTA 块幂等：off < recv_size 视为重复块（ACK 丢失），不重写 Flash，直接回 OK + recv_size；
 *        off > recv_size 才回 ERR_RANGE。应答必须带 4 字节 recv_size 供上位机重对齐。
 * [约束] 擦 128KB 扇区期间 CPU 停摆（单 Bank 不能边擦边取指），进入前必须先喂 IWDG。
 * [约束] 切 Bank 后不可立即复位：应答帧仍在发送寄存器中（264B 约 23ms）。
 * [约束] OTA 空闲超时 10s 退回 FAIL；START 只接受 IDLE/DONE/FAIL，否则板子锁死只能复位。
 */
"""

H['miniweather/BSP/Src/esp8266.c'] = """/* ESP8266 AT 驱动（USART3 PB10/PB11，115200）
 * [约束] 初始化阶段独占 USART3（阻塞查询）；Comm_Init() 之后本驱动不得再碰 huart3。
 * [约束] 调用顺序必须是 ESP8266_Init() -> Comm_Init()，反了会冲突。
 * [约束] 进透传后 AT 不再被解析，退出需发 "+++"（前后各留 1s 静默）。
 * [约束] ESP8266_BootCapture() 必须在 main() 中尽早调用：模块上电约 300ms 就吐完 bootlog。
 * [约束] ESP_PROOF_LINK 必须为 0：透传态下的 AT 诊断会把数据发进 TCP。
 */
"""

H['miniweather/BSP/Inc/esp8266.h'] = """/* [约束] ESP_WIFI_SSID / ESP_WIFI_PWD / ESP_TCP_IP / ESP_TCP_PORT 为占位符，必须改。
 * [约束] 只支持 2.4GHz + WPA2（不支持 WPA3）；热点属公用网络时需放行端口入站。
 * [约束] P2 跳线帽必须纵向插 GBC 侧（左 4<->6、右 3<->5）；插 RS232 侧 USART3 收不到数据。
 * [约束] 联调协议时必须拔掉 ESP8266（模块 TXD 与 USB-TTL TXD 会同时驱动 PB11）。
 */
"""

H['miniweather/BSP/Src/protocol.c'] = """/* 自定义协议：AA 55 | CMD(2B BE, bit15=1 为应答) | LEN(2B BE) | DATA(N) | CRC16(2B BE)
 * 整帧 8+N 字节，N<=256
 * CRC16/IBM：poly 0x8005（反射 0xA001），init 0xFFFF，右移，结果不取反
 * [约束] CRC 计算范围 = 命令字..数据末尾（4+N 字节，不含帧头）
 * 自检：CRC16("123456789") == 0x4B37
 */
"""

H['miniweather/BSP/Inc/protocol.h'] = H['miniweather/BSP/Src/protocol.c']

H['host/protocol.h'] = """/* 帧格式：AA 55 | CMD(2B BE, bit15=1 应答) | LEN(2B BE) | DATA(N) | CRC16(2B BE)
 * CRC16/IBM：poly 0x8005（反射 0xA001），init 0xFFFF，右移，结果不取反
 * [约束] CRC 范围 = 命令字..数据末尾（不含帧头）；自检 CRC16("123456789") == 0x4B37
 * [约束] 固件按内容识别：以 ':' 开头按 Intel HEX 解析，否则按裸 bin。
 */
"""

H['host/protocol.cpp'] = """/* 上位机协议层：组帧 / CRC16 / 固件加载（bin 与 Intel HEX 均支持）
 * [约束] Intel HEX 必须解析地址记录与校验和；解析结果须与 fromelf 产出的 .bin 逐字节一致。
 * [约束] OTA 只发裸 .bin；发 .hex 会导致长度超限与 CRC32 不符。
 */
"""

H['host/mainwindow.cpp'] = """/* Qt 上位机主窗口
 * [约束] OTA 块超时 3000ms / 重试 4 次 / 重试间隔 150ms（串口直连的 1.5s/3 次在 WiFi 下会误判）。
 * [约束] 重传必须复用同一个 off，下位机按 off 判幂等。
 * [约束] OTA 时序：Start -> 等擦除 -> 逐块 -> Finish(CRC32) -> 切 Bank，每步等应答。
 */
"""

H['miniweather/BSP/Src/flash_if.c'] = """/* 内部 Flash 擦写（HAL 版，App 侧）
 * [约束] 擦除前必须 __HAL_FLASH_CLEAR_FLAG 清掉所有挂起标志。
 * [约束] FLASH_EraseInitTypeDef.Banks 必须显式填 FLASH_BANK_1，不填会静默失败。
 * [约束] 写前目标区域必须已擦成 0xFF；采用 TYPEPROGRAM_BYTE 字节编程。
 * [约束] 擦/写前解锁、完成后上锁。
 */
"""

H['bootloader/Src/boot.c'] = """/* Bootloader 主流程：判标志 -> 先验后擦 -> 搬运 -> 再验 -> 跳转
 * [约束] 本工程跑 HSI 16MHz（非 168MHz），USART BRR 须按 16MHz 计算。
 * [约束] 跳转四步：__disable_irq() -> 停 SysTick -> SCB->VTOR + MSP -> 跳转。
 * [约束] App 必须在自身开头 __enable_irq()，否则 PRIMASK=1 被带入，HAL_Delay 死等。
 * [约束] 搬运失败必须保留 UPGRADE_PENDING 并 boot_count++，超 BOOT_COUNT_MAX 才放弃重试。
 * [约束] 必须先校验源固件 CRC32 再擦运行区，校验失败绝不写入。
 */
"""

H['bootloader/Src/flash_boot.c'] = """/* Bootloader 专用 Flash 驱动（直接操作寄存器，不用 HAL）
 * [约束] 解锁：FLASH->KEYR 连写 0x45670123、0xCDEF89AB。
 * [约束] 擦除最小单位是整个扇区；写前目标必须已擦除。
 */
"""

H['bootloader/Inc/crc32.h'] = """/* CRC32/ISO-HDLC：poly 0x04C11DB7（反射 0xEDB88320），
 * init 0xFFFFFFFF，结果异或 0xFFFFFFFF，右移
 * [约束] 算法必须与 App 侧 BSP/Src/flash_if.c 的 Crc32Calc 完全一致。
 */
"""

H['bootloader/Inc/boot_cfg.h'] = """/* [约束] BOOT_LOG_DELAY_MS 联调完成后改 0（该延时每次上电都要等）。
 * [约束] 上电按住 KEY0(PE4，按下为低) 可强制重装一次。
 */
"""

H['miniweather/Core/Src/main.c'] = """/* [约束] IWDG：LSI / Prescaler 64 / Reload 2000 约 4.0s，必须在 osKernelStart() 之前启动。
 * [约束] 搭配 Bootloader 时必须 SCB->VTOR = 0x08010000，并在开头 __enable_irq()。
 * [约束] 时钟：HSE 8MHz -> PLLM 8 / PLLN 336 / PLLP 2 -> SYSCLK 168MHz，
 *        HCLK 168MHz，APB1 42MHz，APB2 84MHz。
 * [约束] 顺序：delay_init -> LCD_Init；ESP8266_Init() -> Comm_Init()。
 * [约束] ESP8266_BootCapture() 必须尽早调用（模块上电约 300ms 吐完 bootlog）。
 * [约束] USART1(CH340) 仅用于 printf，自定义协议跑在 USART3，发错口必然超时。
 */
"""

H['miniweather/Core/Src/freertos.c'] = """/* 任务栈与优先级：defaultTask(Normal,128) / TaskSensor(High,512) /
 * TaskDisplay(AboveNormal,512) / TaskComm(Normal,1024)
 * [约束] 栈大小单位是「字」不是字节；configTOTAL_HEAP_SIZE = 15360。
 * [约束] osThreadCreate() 返回值必须检查：heap 不足返回 NULL，不报错、直接进 HardFault。
 * [约束] 喂狗在 vApplicationIdleHook()：该函数禁止阻塞、禁止 printf、禁止等信号量。
 * [约束] 传感器数据结构必须清零初始化，否则会用栈垃圾当真值。
 * [约束] TaskComm 周期 5ms；n==0 时也要调用 Proto_Tick。
 * [约束] 禁止在 stm32f4xx_it.c 手写 SysTick_Handler / SVC / PendSV（FreeRTOSConfig.h 已宏改名）。
 */
"""

H['miniweather/Core/Src/usart.c'] = """/* USART1(PA9/PA10, 115200) = printf 调试；USART3(PB10/PB11, 115200) = 自定义协议 + OTA
 * [约束] USART3 RX(PB11) 必须开上拉：模块 TXD 为弱驱动，悬空会被噪声误判成起始位。
 * [约束] USART3 接收为 DMA1_Stream1 通道 4 循环模式；取 NDTR 用 huart3.hdmarx。
 */
"""

H['miniweather/BSP/Src/sgp30.c'] = """/* SGP30 气体传感器驱动（I2C 地址 0x58）
 * [约束] 不支持时钟拉伸，测量后必须 delay_ms(12)（手册给的是 max 12ms，勿用 typ 10ms）。
 * [约束] 必须每 1 秒测量一次，否则内部基线算法退化。
 * [约束] 输出为 CO2eq（CO2 当量），不是 CO2 浓度。
 * [约束] baseline 存取顺序相反：Get(CO2, TVOC)，Set(TVOC, CO2)；存 AT24C02，7 天有效。
 */
"""

H['miniweather/BSP/Src/shtc3.c'] = """/* SHTC3 温湿度传感器驱动（I2C 地址 0x70，HAL 传 0x70<<1）
 * 命令 0x7CA2（Normal + 时钟拉伸开 + 温度优先）；读 6 字节：T(2)+CRC(1) 与 RH(2)+CRC(1)
 * 换算：T = -45 + 175*raw/65536；RH = 100*raw/65536
 * CRC-8：poly 0x31，init 0xFF，不反射；自检 CRC(0x00) = 0xAC
 * [约束] 调用方必须检查返回值，否则会拿旧数据或乱码当真值。
 */
"""

H['miniweather/BSP/Src/bmp280.c'] = """/* BMP280 / BME280 驱动（I2C 地址 0x76）
 * 芯片 ID：0x58 = BMP280，0x60 = BME280（丝印无法区分，两个都收）
 * 校准参数 0x88~0x9F 共 24 字节；数据 20 位需 >>4；ctrl_meas = 0x25
 * [约束] 气压补偿计算必须用 int64_t，32 位会溢出。
 */
"""

H['miniweather/BSP/Src/servo.c'] = """/* 舵机驱动（TIM3_CH1 = PA6，50Hz，PSC=83 / ARR=19999，1 计数 = 1us）
 * [约束] MX_TIM3_Init() 只做配置，必须再调 HAL_TIM_PWM_Start()，否则 PA6 恒为 0V。
 */
"""

H['miniweather/BSP/Src/beep.c'] = """/* 蜂鸣器驱动（PF8，有源，音调固定）
 * [约束] BEEP_Alarm() 为阻塞式，FreeRTOS 任务中禁止调用（会卡住任务并触发看门狗）。
 */
"""

H['miniweather/BSP/Src/relay.c'] = """/* 继电器驱动（IN = PE5）
 * [约束] 必须共地。
 * [约束] 严禁接 220V 市电。
 */
"""

H['miniweather/BSP/Src/uart_dma_rx.c'] = """/* USART3 DMA 循环接收环形缓冲
 * [约束] IDLE 中断默认关闭：协议自带长度字段与 CRC16，轮询 NDTR 已足够。
 * [约束] 必须检查启动返回值：ESP 初始化失败会打断 DMA 启动，是「能发不能收」的头号原因。
 */
"""

H['miniweather/BSP/Src/ai_client.c'] = """/* AI 顾问：板端 JSON 上行 -> PC 调大模型 -> 建议文本下行 -> LCD 显示
 * [约束] USART1_IRQHandler 定义在本文件，绕开 HAL_UART_IRQHandler 直读寄存器。
 * [约束] AI_Report() 会往 USART3 写入 JSON，OTA 期间必须停止上报，否则与固件块抢缓冲。
 */
"""

H['miniweather/BSP/Src/app.c'] = """/* 应用层：上电自检 / 屏幕界面 / 按键交互 / 自动联动决策
 * [约束] 手动操作后 APP_MANUAL_HOLD_S(30s) 内自动逻辑不介入。
 * [约束] 行槽 y 坐标一律用枚举，禁止硬编码常量。
 */
"""

H['bootloader/Src/main.c'] = """/* Bootloader 入口
 * [约束] 不配置时钟：跑在复位默认 HSI 16MHz（SystemInit 不会调 SetSysClock()）。
 * [约束] Keil 工程 Options -> C/C++ -> Define 必须有 STM32F407xx。
 */
"""

H['miniweather/Core/Src/delay.c'] = """/* 延时（裸机用 SysTick；OS 下由 delay_osrunning / delay_ostickspersec /
 * delay_osschedlock 三个宏接管）
 */
"""

ok = skip = 0
for fp, head in H.items():
    fp = fp.replace('\\', '/')
    if not os.path.exists(fp):
        print('[MISS]', fp); skip += 1; continue
    with io.open(fp, 'r', encoding='utf-8', errors='surrogateescape') as f:
        src = f.read()
    if src.startswith('/* [') or '[约束]' in src.split('\n')[0:3].__str__():
        pass
    with io.open(fp, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
        f.write(head + src)
    ok += 1
print('插入 %d 个文件，缺失 %d 个' % (ok, skip))
