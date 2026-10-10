#ifndef __COMM_H__
#define __COMM_H__

#include <stdint.h>
#include "flash_if.h"       /* OTA 用的 A/B 区地址、扇区大小都从这儿来 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 可调参数 ---------------- */

/* TaskComm 的调用周期（毫秒）。
   依据第 10 节：115200 下 256 字节缓冲绕一圈 22.2ms，
   取 5ms 时最坏积压 58 字节，余量 4.4 倍。改大就要加大缓冲。 */
#ifndef COMM_TASK_PERIOD_MS
#define COMM_TASK_PERIOD_MS     5u
#endif

/* 是否跑在 FreeRTOS 下（决定临界区用 taskENTER_CRITICAL 还是 __disable_irq）。
   裸机调试时把它改成 0 即可。 */
#ifndef COMM_USE_FREERTOS
#define COMM_USE_FREERTOS       1
#endif

/* 取数据的临时缓冲。必须 >= 一轮可能积累的最大字节数，
   取 URX_BUF_SIZE(256) 就等于"不管积压多少都能一次拿完"。 */
#define COMM_RX_TMP_SIZE        256u

/* 发送缓冲：最大帧 = 8 + 256 = 264 */
#define COMM_TX_BUF_SIZE        270u

/* OTA 相关。地址直接取自共享契约（shared/flash_layout.h），
   这里只是换个 OTA_ 前缀，方便协议层阅读 —— 不是第二份定义。 */
#define OTA_BANK_A_ADDR         BANK_A_ADDR             /* 0x08060000，S7+S8 */
#define OTA_BANK_B_ADDR         BANK_B_ADDR             /* 0x080A0000，S9+S10 */
#define OTA_BANK_SIZE           BANK_SIZE               /* 256KB */
#define OTA_BLOCK_DATA_MAX      128u                    /* 单块固件数据上限 */
#define OTA_SECTOR_SIZE         (128u * 1024u)          /* F407 Sector7~10 每个 128KB */

#define FIRMWARE_VERSION        "v1.1.3"

typedef struct {
    int16_t  temp_01;      /* 温度 ×10，256 = 25.6 ℃  */
    uint16_t humi_01;      /* 湿度 ×10，602 = 60.2 %RH */
    uint32_t press_pa;     /* 气压，单位 Pa            */
    uint16_t co2_ppm;      /* CO2 当量，ppm            */
    uint16_t tvoc_ppb;     /* TVOC，ppb                */
    uint32_t tick;         /* 快照序号，0 = 还没采到过数据 */
} SensorSnapshot_t;

/* 设备 ID（CMD_CTRL_ACTUATOR 的第 1 个参数） */
#define DEV_SERVO               0x01u   /* 舵机 / 开窗机构 */
#define DEV_RELAY               0x02u   /* 继电器 / 风扇   */
#define DEV_BEEP                0x03u   /* 蜂鸣器          */

typedef enum {
    OTA_IDLE = 0,       /* 空闲，没有升级任务 */
    OTA_ERASING,        /* 正在按扇区擦除（由 Comm_OTATick 推进） */
    OTA_READY,          /* 擦完，等待接收固件块 */
    OTA_RECEIVING,      /* 正在收块 */
    OTA_DONE,           /* 收完且 CRC32 校验通过 */
    OTA_FAIL            /* 失败（查 ota_err 看原因） */
} OtaState;

typedef struct {
    OtaState state;
    uint32_t bank_addr;     /* 目标 Bank 起始地址 */
    uint32_t total_size;    /* 固件总字节数 */
    uint32_t recv_size;     /* 已收字节数 */
    uint16_t total_blocks;  /* 总包数 */
    uint16_t recv_blocks;   /* 已收包数 */
    uint32_t crc_expect;    /* 上位机给的目标 CRC32 */
    uint32_t crc_calc;      /* 边收边算的 CRC32 */
    uint32_t erase_addr;    /* 下一个要擦的扇区地址 */
    uint32_t erase_end;     /* 擦除结束地址（不含） */
    uint32_t last_ms;       /* 最后一次"有进展"的时刻（空闲超时判定用） */
    uint8_t  err;           /* 失败原因（错误码） */
    uint8_t  bank;          /* 目标 Bank 号 0=A 1=B */
} OtaCtx_t;

#ifndef OTA_IDLE_TIMEOUT_MS
#define OTA_IDLE_TIMEOUT_MS         (10000u)
#endif

/* ---------------- 本层全局上下文 ---------------- */
typedef struct {
    SensorSnapshot_t sensor;      /* 传感器快照（TaskSensor 写，本层读） */
    uint32_t         uptime_s;    /* 运行秒数（心跳回包用） */
    OtaCtx_t         ota;         /* OTA 状态机 */

    /* 统计 */
    uint32_t         tx_frames;   /* 已发出的应答帧数 */
    uint32_t         unknown_cmd; /* 收到未知命令的次数 */
    uint32_t         ota_dup;     /* OTA 重复块次数（上位机重发，下位机幂等跳过） */
    uint32_t         ota_resync;  /* OTA 偏移错位次数（回 ERR_RANGE + 真实进度） */
} Comm_t;

/* ---------------- API ---------------- */

/* 初始化：复位 OTA 状态机、清零统计、启动 DMA 接收。上电调一次 */
void     Comm_Init(void);

/* 供 TaskSensor 调用：更新传感器快照（内部会进临界区，保证不被撕裂） */
void     Comm_UpdateSensor(const SensorSnapshot_t *snap);
/* 读快照。两边都是临界区整块拷贝，缺任何一边保护都不成立。
   ★ 禁止直接写 Comm_GetCtx()->sensor —— 那会绕过读侧的锁。 */
void     Comm_GetSensorSnapshot(SensorSnapshot_t *out);

/* 供 TaskSensor / 心跳任务调用：更新运行秒数 */
void     Comm_SetUptime(uint32_t sec);

/* ★ TaskComm 的核心循环体：取数据 → 喂协议 → 推进 OTA。每 5ms 调一次 */
void     Comm_Poll(void);

/* 命令处理入口（协议层 CRC 通过后调用）。也暴露出来方便单测直接喂帧 */
void     Comm_OnFrame(uint16_t cmd, const uint8_t *payload, uint16_t len);

/* 组包并发送一条应答。err = 错误码（数据区第 1 字节） */
uint16_t Comm_SendReply(uint16_t req_cmd, uint8_t err,
                        const uint8_t *data, uint16_t len);

/* OTA 状态机推进（Comm_Poll 内部会调，也可以单独调来加速擦除） */
void     Comm_OTATick(void);

void     Comm_GetSensorSnapshot(SensorSnapshot_t *out);

/* 供 main 或别的任务查询当前 OTA 状态（比如 LCD 上显示进度） */
OtaState Comm_GetOtaState(void);
uint32_t Comm_GetOtaProgress(void);   /* 返回已收字节数 */

/* 取内部上下文（调试/自测用，比如打印统计） */
Comm_t  *Comm_GetCtx(void);

/* ---------------- 平台适配函数（本层唯一"知道硬件"的三个函数） ----------------
   它们把 HAL / 具体外设挡在业务代码外面：
     - 换 MCU 只改这三个函数
     - PC 仿真时把它们替换成桩函数即可，comm.c 一个字不用改 */
int      Comm_FlashEraseSector(uint32_t addr);                          /* 0=成功 */
int      Comm_FlashWrite(uint32_t addr, const uint8_t *data, uint32_t len);
void     Comm_FlashRead(uint32_t addr, uint8_t *data, uint32_t len);
int      Comm_ActuatorSet(uint8_t dev, uint8_t act);                    /* 0=成功 */

/* CRC32（IEEE 802.3）：poly 0xEDB88320，init 0xFFFFFFFF，xorout 0xFFFFFFFF
   自检：CRC32("123456789") = 0xCBF43926 */
uint32_t CRC32_Step(uint32_t crc, uint8_t b);          /* 流式单字节 */
uint32_t CRC32_Calc(const uint8_t *d, uint32_t len);   /* 一次性 */

#ifdef __cplusplus
}
#endif

#endif /* __COMM_H__ */
