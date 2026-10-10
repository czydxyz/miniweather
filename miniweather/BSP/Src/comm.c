/* 协议落地层（AA55 | CMD | LEN | DATA | CRC16）
 * [约束] 共享数据必须成对使用 Comm_UpdateSensor() / Comm_GetSensorSnapshot()，
 *        禁止任何地方直接访问 Comm_GetCtx()->xxx。
 * [约束] OTA 块幂等：off < recv_size 视为重复块（ACK 丢失），不重写 Flash，直接回 OK + recv_size；
 *        off > recv_size 才回 ERR_RANGE。应答必须带 4 字节 recv_size 供上位机重对齐。
 * [约束] 擦 128KB 扇区期间 CPU 停摆（单 Bank 不能边擦边取指），进入前必须先喂 IWDG。
 * [约束] 切 Bank 后不可立即复位：应答帧仍在发送寄存器中（264B 约 23ms）。
 * [约束] OTA 空闲超时 10s 退回 FAIL；START 只接受 IDLE/DONE/FAIL，否则板子锁死只能复位。
 */
/*
 * comm.c —— 协议落地层实现
 * 这一层是「协议层 → 业务」的中间人。写它最容易犯的错，都在注释里标了 ★
  */

#include "comm.h"
#include "protocol.h"
#include "uart_dma_rx.h"
#include "flash_if.h"
#include "ai_client.h"      /* AI 建议文本旁路：AI_FeedBytes() */
#include <string.h>

#include "main.h"
#include "usart.h"
#include "servo.h"
#include "relay.h"
#include "beep.h"
#include <stdio.h>

#if COMM_USE_FREERTOS
#include "FreeRTOS.h"
#include "task.h"
#define COMM_ENTER_CRITICAL()       taskENTER_CRITICAL()
#define COMM_EXIT_CRITICAL()        taskEXIT_CRITICAL()
#else
/* 裸机：直接关总中断。注意这会关掉所有中断，临界区里别干耗时的事 */
#define COMM_ENTER_CRITICAL()       __disable_irq()
#define COMM_EXIT_CRITICAL()        __enable_irq()
#endif

/* ---------------- 平台适配宏 ----------------
   ★ 这一组宏是"平台适配层"：业务代码只认 COMM_TX / COMM_FLASH_WRITE，
     底下到底是 HAL、是 DMA、还是以太网，改这一处就行。 */

/* 发送：目前用阻塞发送。
   敢用阻塞的理由：应答最多 264 字节 @115200 ≈ 23ms，且上位机是"发一条等一条"，
   发应答期间不会有新数据进来。将来要做主动上报（比如每 100ms 推一次数据）
   就必须改成 HAL_UART_Transmit_DMA + 忙标志，否则会把任务拖死。 */
#define COMM_TX(p, n)               HAL_UART_Transmit(&huart3, (uint8_t *)(p), (n), 100)

#ifndef COMM_RX_DEBUG
#define COMM_RX_DEBUG               1
#endif

/* Flash：真正的实现在 flash_if.c（地址→扇区换算、解锁/上锁都在那里） */
#define COMM_FLASH_ERASE(a)         Comm_FlashEraseSector(a)
#define COMM_FLASH_WRITE(a, p, n)   Comm_FlashWrite((a), (p), (n))
#define COMM_FLASH_READ(a, p, n)    Comm_FlashRead((a), (p), (n))

#ifndef COMM_IWDG_RELOAD
#define COMM_IWDG_RELOAD()          IWDG_Feed()
#endif

/* 执行器：把"设备号+动作"翻译成具体驱动的调用 */
#define COMM_ACTUATOR(dev, act)     Comm_ActuatorSet((dev), (act))

#define COMM_NOW_MS()               HAL_GetTick()

/* 小工具：把 32 位无符号整数按【大端】塞进 4 字节数组。
   协议里所有多字节字段都是大端（第 9 节定稿），统一走这个宏，
   免得每处手写 4 行移位时把 >>,>> 打错 —— 这种错在串口上表现为
   "进度条跳到天文数字"，很难一眼看出来。 */
#define U32BE(buf, v)   do { \
        (buf)[0] = (uint8_t)((uint32_t)(v) >> 24); \
        (buf)[1] = (uint8_t)((uint32_t)(v) >> 16); \
        (buf)[2] = (uint8_t)((uint32_t)(v) >> 8);  \
        (buf)[3] = (uint8_t)((uint32_t)(v));       \
    } while (0)

uint32_t CRC32_Step(uint32_t crc, uint8_t b)
{
    uint8_t i;
    crc ^= b;
    for (i = 0; i < 8; i++) {
        /* 右移 + 反射多项式 0xEDB88320（是 0x04C11DB7 的位反转） */
        if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
        else          crc = (crc >> 1);
    }
    return crc;
}

uint32_t CRC32_Calc(const uint8_t *d, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    for (i = 0; i < len; i++) crc = CRC32_Step(crc, d[i]);
    return crc ^ 0xFFFFFFFFu;
}

/* 二、全局上下文与初始化 */
static Comm_t   g_comm;
static ProtoCtx g_proto;                       /* 协议解析器实例（每个通道一个） */
static UartRx_t g_urx;                         /* 串口环形接收（第 10 节那个） */
static uint8_t  g_rx_tmp[COMM_RX_TMP_SIZE];
static uint8_t  g_tx_buf[COMM_TX_BUF_SIZE];

void Comm_Init(void)
{
    memset(&g_comm, 0, sizeof(g_comm));
    memset(&g_tx_buf, 0, sizeof(g_tx_buf));
    g_comm.ota.state = OTA_IDLE;
    Proto_Init(&g_proto, Comm_OnFrame);        /* ★ 注册回调：收满一帧就调 Comm_OnFrame */
    UartRx_Start(&g_urx);                      /* 启动 DMA 循环接收，之后永不停 */
    UartRx_Register(&g_urx);                   /* 登记给中断钩子（TC 计数 / ORE 恢复用） */
}

Comm_t *Comm_GetCtx(void) { return &g_comm; }

/*
 * 三、平台适配函数：本层唯一"知道硬件"的三个函数
 *    它们把 HAL / 具体外设挡在业务代码外面，换 MCU 只改这一处
  */
int Comm_FlashEraseSector(uint32_t addr)
{
    return FLASH_IF_EraseSectorByAddr(addr);
}

int Comm_FlashWrite(uint32_t addr, const uint8_t *data, uint32_t len)
{
    return FLASH_IF_Write(addr, data, len);
}

void Comm_FlashRead(uint32_t addr, uint8_t *data, uint32_t len)
{
    FLASH_IF_Read(addr, data, len);
}

/* 把协议里的"设备号+动作"翻译成真实驱动调用。
   ★ 只做"立刻能完成"的事：置 GPIO / 设 CCR / 起一个非阻塞状态机。
     绝对不能在这里 delay —— 会挡住整个 TaskComm。 */
int Comm_ActuatorSet(uint8_t dev, uint8_t act)
{
    switch (dev) {
    case DEV_SERVO:
        if (act) Servo_Open();      /* 开窗：360 舵机=正转定时，180 舵机=转 90° */
        else     Servo_Close();
        break;

    case DEV_RELAY:
        RELAY_Set(act);             /* 1=吸合（开风扇）0=断开 */
        break;

    case DEV_BEEP:
        if (act) BEEP_AlarmStart(3, 200, 200);   /* 响 3 声，非阻塞 */
        else     BEEP_AlarmStop();
        break;

    default:
        return 1;
    }
    return 0;
}

void Comm_UpdateSensor(const SensorSnapshot_t *snap)
{
    if (snap == NULL) return;
    COMM_ENTER_CRITICAL();
    g_comm.sensor = *snap;
    COMM_EXIT_CRITICAL();
}

void Comm_GetSensorSnapshot(SensorSnapshot_t *out)
{
    if (out == NULL) return;
    COMM_ENTER_CRITICAL();
    *out = g_comm.sensor;
    COMM_EXIT_CRITICAL();
}

void Comm_SetUptime(uint32_t sec)
{
    g_comm.uptime_s = sec;
}

/* 五、发送 —— 应答帧怎么出去 */
uint16_t Comm_SendReply(uint16_t req_cmd, uint8_t err,
                        const uint8_t *data, uint16_t len)
{
    uint16_t n;

    /* Proto_BuildAck 会把数据区第 1 字节填成 err，命令字自动置 bit15 */
    n = Proto_BuildAck(req_cmd, err, data, len, g_tx_buf, COMM_TX_BUF_SIZE);
    if (n == 0) return 0;

    COMM_TX(g_tx_buf, n);
    g_comm.tx_frames++;
    return n;
}


/* --- 6.1 查版本 --- */
static void HandleGetVersion(const uint8_t *p, uint16_t len)
{
    (void)p;
    if (len != 0) { Comm_SendReply(CMD_GET_VERSION, PROTO_ERR_PARAM, NULL, 0); return; }
    Comm_SendReply(CMD_GET_VERSION, PROTO_ERR_OK,
                   (const uint8_t *)FIRMWARE_VERSION,
                   (uint16_t)strlen(FIRMWARE_VERSION));
}

/* --- 6.2 心跳 --- */
static void HandleHeartbeat(const uint8_t *p, uint16_t len)
{
    uint8_t d[5];
    (void)p;
    if (len != 0) { Comm_SendReply(CMD_HEARTBEAT, PROTO_ERR_PARAM, NULL, 0); return; }
    d[0] = (uint8_t)(g_comm.uptime_s >> 24);
    d[1] = (uint8_t)(g_comm.uptime_s >> 16);
    d[2] = (uint8_t)(g_comm.uptime_s >> 8);
    d[3] = (uint8_t)(g_comm.uptime_s);
    d[4] = (uint8_t)g_comm.ota.state;      /* 顺手把 OTA 状态带回去 */
    Comm_SendReply(CMD_HEARTBEAT, PROTO_ERR_OK, d, 5);
}

/* --- 6.3 读传感器 --- */
static void HandleReadSensor(const uint8_t *p, uint16_t len)
{
    uint8_t        d[13];
    SensorSnapshot_t s;
    (void)p;
    if (len != 0) { Comm_SendReply(CMD_READ_SENSOR, PROTO_ERR_PARAM, NULL, 0); return; }

    /* 拷贝一份快照再慢慢组包（避免组包途中被改写） */
    COMM_ENTER_CRITICAL();
    s = g_comm.sensor;
    COMM_EXIT_CRITICAL();

    d[0]  = (uint8_t)((uint16_t)s.temp_01 >> 8);
    d[1]  = (uint8_t)((uint16_t)s.temp_01);
    d[2]  = (uint8_t)(s.humi_01 >> 8);
    d[3]  = (uint8_t)(s.humi_01);
    d[4]  = (uint8_t)(s.press_pa >> 24);
    d[5]  = (uint8_t)(s.press_pa >> 16);
    d[6]  = (uint8_t)(s.press_pa >> 8);
    d[7]  = (uint8_t)(s.press_pa);
    d[8]  = (uint8_t)(s.co2_ppm >> 8);
    d[9]  = (uint8_t)(s.co2_ppm);
    d[10] = (uint8_t)(s.tvoc_ppb >> 8);
    d[11] = (uint8_t)(s.tvoc_ppb);
    d[12] = (s.tick == 0) ? 0 : 1;        /* 0 = 还没采到过数据 */
    Comm_SendReply(CMD_READ_SENSOR, PROTO_ERR_OK, d, 13);
}

/* --- 6.4 控制执行器 --- */
static void HandleCtrlActuator(const uint8_t *p, uint16_t len)
{
    uint8_t dev, act;
    if (len != 2) { Comm_SendReply(CMD_CTRL_ACTUATOR, PROTO_ERR_PARAM, NULL, 0); return; }
    dev = p[0];
    act = p[1];

    /* 参数合法性：只认 3 种设备、动作只认 0/1 */
    if ((dev != DEV_SERVO && dev != DEV_RELAY && dev != DEV_BEEP) || act > 1) {
        Comm_SendReply(CMD_CTRL_ACTUATOR, PROTO_ERR_PARAM, NULL, 0);
        return;
    }

    COMM_ACTUATOR(dev, act);
    Comm_SendReply(CMD_CTRL_ACTUATOR, PROTO_ERR_OK, NULL, 0);
}

/* --- 6.5 开始升级 --- */
static void HandleUpdateStart(const uint8_t *p, uint16_t len)
{
    OtaCtx_t *o = &g_comm.ota;
    uint32_t  size;
    uint16_t  blocks;
    uint32_t  crc;
    uint8_t   bank;

    /* ★ 向后兼容：第 9 节定稿是 10 字节（无 Bank），这里额外接受 11 字节
       （第 11 字节 = Bank 号）。老上位机不发第 11 字节也能用，默认 A 区。 */
    if (len != 10 && len != 11) {
        Comm_SendReply(CMD_UPDATE_START, PROTO_ERR_PARAM, NULL, 0); return;
    }
    size   = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    blocks = (uint16_t)(((uint16_t)p[4] << 8) | p[5]);
    crc    = ((uint32_t)p[6] << 24) | ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 8) | p[9];
    bank   = (len == 11) ? p[10] : 0;

    if (size == 0 || size > OTA_BANK_SIZE) {
        Comm_SendReply(CMD_UPDATE_START, PROTO_ERR_RANGE, NULL, 0); return;
    }
    if (bank > 1) {
        Comm_SendReply(CMD_UPDATE_START, PROTO_ERR_PARAM, NULL, 0); return;
    }
    /* 已有升级在进行中：拒绝新的 Start */
    if (o->state != OTA_IDLE && o->state != OTA_DONE && o->state != OTA_FAIL) {
        Comm_SendReply(CMD_UPDATE_START, PROTO_ERR_BUSY, NULL, 0); return;
    }

    /* ★ 这里只登记参数，不擦除！擦除交给 Comm_OTATick 一个扇区一个扇区来 */
    memset(o, 0, sizeof(*o));
    o->bank_addr    = (bank == 0) ? OTA_BANK_A_ADDR : OTA_BANK_B_ADDR;
    o->bank         = bank;
    o->total_size   = size;
    o->total_blocks = blocks;
    o->crc_expect   = crc;
    o->crc_calc     = 0xFFFFFFFFu;      /* CRC32 的初值（最后要再异或一次） */
    o->erase_addr   = o->bank_addr;
    /* 擦到"够放这个固件"为止，不用把整个 256KB 都擦了 */
    o->erase_end    = o->bank_addr + ((size + OTA_SECTOR_SIZE - 1) / OTA_SECTOR_SIZE) * OTA_SECTOR_SIZE;
    o->last_ms      = COMM_NOW_MS();        /* 起表：空闲超时从现在开始算 */
    o->state        = OTA_ERASING;

    Comm_SendReply(CMD_UPDATE_START, PROTO_ERR_OK, NULL, 0);
}

/* --- 6.6 固件块 --- */
static void HandleUpdateBlock(const uint8_t *p, uint16_t len)
{
    OtaCtx_t *o = &g_comm.ota;
    uint32_t  off, data_len;
    const uint8_t *data;
    uint8_t   rsp[4];
    int       r;

    /* 状态检查：没 Start 就发块 → BUSY；已经结束 → BUSY */
    if (o->state != OTA_READY && o->state != OTA_RECEIVING) {
        Comm_SendReply(CMD_UPDATE_BLOCK, PROTO_ERR_BUSY, NULL, 0); return;
    }
    if (len < 4 || (uint32_t)(len - 4) > OTA_BLOCK_DATA_MAX) {
        Comm_SendReply(CMD_UPDATE_BLOCK, PROTO_ERR_PARAM, NULL, 0); return;
    }

    o->last_ms = COMM_NOW_MS();            /* 收到块 = 还活着，刷新空闲超时 */

    off      = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    data     = &p[4];
    data_len = len - 4;

    if (off != o->recv_size) {
        U32BE(rsp, o->recv_size);          /* 无论哪种错位，都把真实进度带回去 */

        if ((off + data_len) <= o->recv_size) {
            /* ② 重复块：什么都不做，直接回 OK。
                  ★ 绝对不能重写 Flash：Flash 写之前要解锁/擦，重复写既耗时
                    又可能把已经写好的数据搞坏（尤其是跨"半字/字"对齐时）。 */
            g_comm.ota_dup++;
            Comm_SendReply(CMD_UPDATE_BLOCK, PROTO_ERR_OK, rsp, 4);
            return;
        }

        /* ③ 真错位：真丢了一块，或偏移算错了。让上位机按 recv_size 续发 */
        g_comm.ota_resync++;
        Comm_SendReply(CMD_UPDATE_BLOCK, PROTO_ERR_RANGE, rsp, 4);
        return;
    }

    /* ① 正常路径（走到这里必然 off == recv_size） */
    if (o->recv_size + data_len > o->total_size) {
        U32BE(rsp, o->recv_size);
        Comm_SendReply(CMD_UPDATE_BLOCK, PROTO_ERR_RANGE, rsp, 4); return;
    }

    /* 写 Flash：128 字节约 0.2ms，在回调里做可以接受 */
    r = COMM_FLASH_WRITE(o->bank_addr + off, data, data_len);
    if (r != 0) {
        o->state = OTA_FAIL;
        o->err   = PROTO_ERR_FLASH;
        Comm_SendReply(CMD_UPDATE_BLOCK, PROTO_ERR_FLASH, NULL, 0);
        return;
    }

    /* 边收边算 CRC32（流式，不需要把整个固件留在 RAM 里） */
    {
        uint32_t i;
        for (i = 0; i < data_len; i++) o->crc_calc = CRC32_Step(o->crc_calc, data[i]);
    }
    o->recv_size  += data_len;
    o->recv_blocks++;
    o->state       = OTA_RECEIVING;

    /* 回"已收字节数"，上位机可以直接拿它画进度条 */
    U32BE(rsp, o->recv_size);
    Comm_SendReply(CMD_UPDATE_BLOCK, PROTO_ERR_OK, rsp, 4);
}

/* --- 6.7 升级完成（算总账） --- */
static void HandleUpdateFinish(const uint8_t *p, uint16_t len)
{
    OtaCtx_t *o = &g_comm.ota;
    uint32_t  crc_remote, crc_local;

    if (len != 4) { Comm_SendReply(CMD_UPDATE_FINISH, PROTO_ERR_PARAM, NULL, 0); return; }
    if (o->state != OTA_RECEIVING) {
        Comm_SendReply(CMD_UPDATE_FINISH, PROTO_ERR_BUSY, NULL, 0); return;
    }

    crc_remote = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    crc_local  = o->crc_calc ^ 0xFFFFFFFFu;    /* 流式 CRC 收尾：取反 */

    if (o->recv_size != o->total_size) {
        o->state = OTA_FAIL; o->err = PROTO_ERR_RANGE;
        Comm_SendReply(CMD_UPDATE_FINISH, PROTO_ERR_RANGE, NULL, 0); return;
    }
    if (crc_remote != crc_local) {
        /* ★ 校验失败要把状态机复位，否则下次 Start 会卡在 RECEIVING */
        o->state = OTA_FAIL; o->err = PROTO_ERR_CRC32;
        Comm_SendReply(CMD_UPDATE_FINISH, PROTO_ERR_CRC32, NULL, 0); return;
    }

    o->state = OTA_DONE;
    Comm_SendReply(CMD_UPDATE_FINISH, PROTO_ERR_OK, NULL, 0);
}

/* --- 6.8 切换 Bank --- */
static void HandleSwitchBank(const uint8_t *p, uint16_t len)
{
    uint8_t   bank;
    BootFlag_t flag;
    int       r;

    if (len != 1) { Comm_SendReply(CMD_SWITCH_BANK, PROTO_ERR_PARAM, NULL, 0); return; }
    bank = p[0];
    if (bank > 1) { Comm_SendReply(CMD_SWITCH_BANK, PROTO_ERR_PARAM, NULL, 0); return; }

    /* ① 先读一次旧标志，保留 boot_count 等历史信息；读不到就全零起步 */
    memset(&flag, 0, sizeof(flag));
    (void)FLASH_IF_ReadBootFlag(&flag);

    /* ② 填新标志。Bootloader 上电看到 upgrade_flag=1 就会搬运然后跳过去 */
    flag.version      = 0x00010000u;            /* v1.0，改成你自己的版本号 */
    flag.active_bank  = bank;
    /* ★ 用共享头里的枚举，不要写裸的 1。
       裸数字是"看起来懂、其实最容易改错"的东西：
       万一哪天 UPGRADE_PENDING 从 1 变成 2，这里不会跟着变，
       Bootloader 就会认为"没有升级任务"，整套 OTA 静默失效。 */
    flag.upgrade_flag = UPGRADE_PENDING;
    flag.boot_count   = 0;
    if (g_comm.ota.state == OTA_DONE && g_comm.ota.bank == bank) {
        /* 刚升级完：把这次的固件信息一并记进去，Boot 可以顺手校验 */
        flag.fw_size  = g_comm.ota.total_size;
        flag.fw_crc32 = g_comm.ota.crc_expect;
    }

    COMM_IWDG_RELOAD();
    r = FLASH_IF_WriteBootFlag(&flag);
    if (r != 0) {
        Comm_SendReply(CMD_SWITCH_BANK, PROTO_ERR_FLASH, NULL, 0);
        return;
    }

    Comm_SendReply(CMD_SWITCH_BANK, PROTO_ERR_OK, NULL, 0);

}

/* --- 命令分发 --- */
void Comm_OnFrame(uint16_t cmd, const uint8_t *payload, uint16_t len)
{
#if COMM_RX_DEBUG
    /* 走到这里 = 帧头、长度、CRC 全对了，物理层和协议层都已确认正常 */
    printf("[COMM  ] FRAME cmd=0x%04X len=%u\r\n", (unsigned int)cmd, (unsigned int)len);
#endif

    switch (cmd) {
    case CMD_GET_VERSION:     HandleGetVersion(payload, len);     break;
    case CMD_HEARTBEAT:       HandleHeartbeat(payload, len);      break;
    case CMD_READ_SENSOR:     HandleReadSensor(payload, len);     break;
    case CMD_CTRL_ACTUATOR:   HandleCtrlActuator(payload, len);   break;
    case CMD_UPDATE_START:    HandleUpdateStart(payload, len);    break;
    case CMD_UPDATE_BLOCK:    HandleUpdateBlock(payload, len);    break;
    case CMD_UPDATE_FINISH:   HandleUpdateFinish(payload, len);   break;
    case CMD_SWITCH_BANK:     HandleSwitchBank(payload, len);     break;
    default:
        /* 未知命令：也要回一条，否则上位机会一直等到超时才知道失败 */
        g_comm.unknown_cmd++;
        Comm_SendReply(cmd, PROTO_ERR_CMD, NULL, 0);
        break;
    }
}

/*
 * 七、OTA 状态机推进
 * 每个 Comm_Poll 周期只擦【一个】扇区。F407 擦 128KB 大约 1~2 秒，
 * 期间 TaskComm 是卡住的 —— 但因为上位机在等 ACK，不会发新数据，
 * 所以不会丢字节；擦之前喂一次看门狗防止复位。
  */
void Comm_OTATick(void)
{
    OtaCtx_t *o = &g_comm.ota;

    /* ★ 空闲超时：收块阶段（READY / RECEIVING）超过 OTA_IDLE_TIMEOUT_MS
         没有任何新块到达，就认为上位机那边已经放弃了（崩溃/拔线/中止）。
         主动退回 FAIL，让下一次 START 能被接受 —— 否则板子会一直卡在
         RECEIVING，第二次升级回 ERR_BUSY，只能按复位才救得回来。 */
    if ((o->state == OTA_READY || o->state == OTA_RECEIVING) &&
        (uint32_t)(COMM_NOW_MS() - o->last_ms) > OTA_IDLE_TIMEOUT_MS) {
        /* ★ 这行日志故意只用 ASCII：ARMCC V5 对源码里的中文会报
              #870-D invalid multibyte character sequence（不是错误，但刷屏）。 */
        printf("[COMM  ] OTA idle timeout %u ms, state %d -> FAIL\r\n",
               (unsigned int)OTA_IDLE_TIMEOUT_MS, (int)o->state);
        o->state = OTA_FAIL;
        o->err   = PROTO_ERR_TIMEOUT;
        return;
    }

    if (o->state != OTA_ERASING) return;

    if (o->erase_addr < o->erase_end) {
        COMM_IWDG_RELOAD();                    /* 擦除耗时长，先喂狗 */
        if (COMM_FLASH_ERASE(o->erase_addr) != 0) {
            o->state = OTA_FAIL;
            o->err   = PROTO_ERR_FLASH;
            return;
        }
        o->erase_addr += OTA_SECTOR_SIZE;
    } else {
        o->state   = OTA_READY;                /* 擦完，可以开始收块了 */
        o->last_ms = COMM_NOW_MS();            /* 超时重新起表 */
    }
}

OtaState Comm_GetOtaState(void)          { return g_comm.ota.state; }
uint32_t Comm_GetOtaProgress(void)       { return g_comm.ota.recv_size; }

void Comm_Poll(void)
{
    uint16_t n;
#if COMM_RX_DEBUG
    uint32_t i;
#endif

    /* ① 先推进 OTA 擦除（可能耗时，放最前面，让后续流程拿到最新状态） */
    Comm_OTATick();

    /* ② 从 DMA 环形缓冲取新数据 */
    n = UartRx_Take(&g_urx, g_rx_tmp, COMM_RX_TMP_SIZE);

#if COMM_RX_DEBUG
    if (n != 0) {
        /* ②-a 原始字节诊断：限流 200ms 打一次，避免高速收数据时刷爆串口 */
        static uint32_t dbg_last_ms = 0;
        uint32_t now = COMM_NOW_MS();
        if ((uint32_t)(now - dbg_last_ms) >= 200) {
            dbg_last_ms = now;
            printf("[COMM  ] RX %u byte(s):", (unsigned int)n);
            for (i = 0; (i < n) && (i < 8); i++) printf(" %02X", g_rx_tmp[i]);
            printf("\r\n");
        }
    }
#endif

    if (n == 0) {
        /* ★ 没数据也要喂一次 Tick，否则"收到一半断了"永远不会超时复位：
              链路在半帧处断开 → 再也收不到字节 → 走不到超时检测
              → 状态机永远卡在 P_ST_DATA，后面所有正确帧全被吞掉。
              现场表现是"用着用着不响应，重启才好"。 */
        Proto_Tick(&g_proto, COMM_NOW_MS());
        return;
    }

    if ((g_proto.state == P_ST_HEAD_HI) &&
        !(n >= 2u && g_rx_tmp[0] == 0xAAu && g_rx_tmp[1] == 0x55u))
    {
        AI_FeedBytes(g_rx_tmp, n);
        /* ★ 仍要推进协议层的超时时钟：否则半帧超时检测永远不触发，
             一条半截帧就能让状态机永久卡死（老 bug，别回头踩）。 */
        Proto_Tick(&g_proto, COMM_NOW_MS());
        return;
    }

    /* ③ 喂给协议层。一整段里可能含多帧，FeedBytes 会逐字节跑状态机，
          每收满一帧就同步回调 Comm_OnFrame() → 里面直接回应答。
          ★ 所以"粘包"在这里天然被处理：连发 3 帧就会回 3 条应答。 */
    Proto_FeedBytes(&g_proto, g_rx_tmp, n);

#if COMM_RX_DEBUG
    /* ③-a 解析结果诊断：只在新出错时打，正常时完全静默。
       err_crc 涨 = 字节收到了但校验不对 → 波特率 / CRC 算法 / 大小端对不上
       err_len 涨 = 长度字段被解成了天文数字 → 八成是波特率错导致的字节错位 */
    {
        static uint32_t last_crc = 0, last_len = 0, last_to = 0;
        if (g_proto.err_crc != last_crc) {
            last_crc = g_proto.err_crc;
            printf("[COMM  ] !! CRC 错 (累计 %u) -> 查波特率/CRC参数\r\n", (unsigned int)last_crc);
        }
        if (g_proto.err_len != last_len) {
            last_len = g_proto.err_len;
            printf("[COMM  ] !! 长度非法 (累计 %u) -> 多半波特率错\r\n", (unsigned int)last_len);
        }
        if (g_proto.err_timeout != last_to) {
            last_to = g_proto.err_timeout;
            printf("[COMM  ] !! 半帧超时复位 (累计 %u)\r\n", (unsigned int)last_to);
        }
    }
#endif

    /* ④ 更新协议层时钟（超时复位的依据） */
    Proto_Tick(&g_proto, COMM_NOW_MS());
}
