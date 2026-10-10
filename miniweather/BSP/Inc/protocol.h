/* 自定义协议：AA 55 | CMD(2B BE, bit15=1 为应答) | LEN(2B BE) | DATA(N) | CRC16(2B BE)
 * 整帧 8+N 字节，N<=256
 * CRC16/IBM：poly 0x8005（反射 0xA001），init 0xFFFF，右移，结果不取反
 * [约束] CRC 计算范围 = 命令字..数据末尾（4+N 字节，不含帧头）
 * 自检：CRC16("123456789") == 0x4B37
 */
#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 用户可裁剪的配置 ---------------- */

/* 数据区最大长度。固件块 = 4 字节偏移 + 128 字节数据 = 132，
   留余量到 256。改大就要同步改 ProtoCtx.buf（RAM 开销）。 */
#ifndef PROTO_MAX_PAYLOAD
#define PROTO_MAX_PAYLOAD       256u
#endif

/* 帧接收超时（毫秒）。超过这个时间没收到新字节，状态机强制回到等帧头。
   目的：一帧收到一半链路断了，不能永远卡在收数据状态。 */
#ifndef PROTO_RX_TIMEOUT_MS
#define PROTO_RX_TIMEOUT_MS     200u
#endif

/* ---------------- 帧相关常量 ---------------- */

#define PROTO_HEAD_HI           0xAAu   /* 帧头第 1 字节（先发） */
#define PROTO_HEAD_LO           0x55u   /* 帧头第 2 字节 */
#define PROTO_HEAD_LEN          2u
#define PROTO_CRC_LEN           2u
#define PROTO_FIXED_LEN         6u      /* 命令2 + 长度2 + CRC2 */
#define PROTO_MIN_FRAME         (PROTO_HEAD_LEN + PROTO_FIXED_LEN)   /* 8，空数据帧 */
#define PROTO_MAX_FRAME         (PROTO_MIN_FRAME + PROTO_MAX_PAYLOAD)

/* ---------------- 命令字 ----------------
   bit15 是"应答标志"：请求帧 0x00xx，应答帧把 bit15 置 1 变成 0x80xx。
   这样看一眼命令字就知道是问还是答，不用额外字段。 */
#define PROTO_ACK_FLAG          0x8000u

#define CMD_GET_VERSION         0x0001u /* 查询固件版本（请求空，应答含版本串） */
#define CMD_HEARTBEAT           0x0002u /* 心跳 / 保活 */
#define CMD_UPDATE_START        0x0003u /* 开始升级：数据=总大小(4)+总包数(2)+CRC32(4) */
#define CMD_UPDATE_BLOCK        0x0004u /* 固件块：数据=偏移(4)+固件数据(<=128) */
#define CMD_UPDATE_FINISH       0x0005u /* 升级完成：数据=整包 CRC32(4) */
#define CMD_SWITCH_BANK         0x0006u /* 切换启动 Bank：数据=Bank 号(1) */
#define CMD_READ_SENSOR         0x0007u /* 读传感器（请求空，应答含温湿压气） */
#define CMD_CTRL_ACTUATOR       0x0008u /* 控制执行器：数据=设备ID(1)+动作(1) */

/* ---------------- 错误码（应答帧数据区第 1 字节） ---------------- */
#define PROTO_ERR_OK            0x00u   /* 成功 */
#define PROTO_ERR_CMD           0x01u   /* 未知命令 */
#define PROTO_ERR_PARAM         0x02u   /* 参数非法（长度/取值不对） */
#define PROTO_ERR_LEN           0x03u   /* 长度字段超出上限 */
#define PROTO_ERR_BUSY          0x04u   /* 设备忙（如正在升级） */
#define PROTO_ERR_FLASH         0x05u   /* Flash 擦写失败 */
#define PROTO_ERR_RANGE         0x06u   /* 偏移越界 */
#define PROTO_ERR_TIMEOUT       0x07u   /* 等待后续包超时 */
#define PROTO_ERR_CRC32         0x08u   /* 整包 CRC32 不匹配 */

/* ---------------- 解析状态机 ---------------- */

typedef enum {
    P_ST_HEAD_HI = 0,   /* 等 0xAA */
    P_ST_HEAD_LO,       /* 等 0x55 */
    P_ST_CMD,           /* 收 2 字节命令字 */
    P_ST_LEN,           /* 收 2 字节长度 */
    P_ST_DATA,          /* 收 N 字节数据 */
    P_ST_CRC            /* 收 2 字节 CRC */
} ProtoState;

/* 解析结果回调：只有在 CRC 校验通过时才被调用 */
typedef void (*ProtoCallback)(uint16_t cmd, const uint8_t *payload, uint16_t len);

/* 协议解析器上下文（每个通信通道一个实例：串口1 / 串口3 / 以太网各一个） */
typedef struct {
    ProtoState  state;                       /* 当前状态 */
    uint16_t    idx;                         /* 当前字段已收字节数 */
    uint16_t    cmd;                         /* 正在收的命令字 */
    uint16_t    len;                         /* 数据区长度 N */
    uint8_t     buf[PROTO_MAX_PAYLOAD];      /* 数据区缓冲 */
    uint16_t    crc_calc;                    /* 边收边算的 CRC（命令+长度+数据） */
    uint16_t    crc_recv;                    /* 收到的 CRC */
    uint32_t    last_ms;                     /* 最后一次收到字节的时刻 */
    uint32_t    now_ms;                      /* 由 Proto_Tick 写入的"当前时刻" */

    /* 统计（调试用，出问题先看这几个数） */
    uint32_t    rx_frames;                   /* 成功收到的帧数 */
    uint32_t    err_crc;                     /* CRC 错的帧数 */
    uint32_t    err_len;                     /* 长度非法的帧数 */
    uint32_t    err_timeout;                 /* 超时重置次数 */

    ProtoCallback cb;                        /* 收满一帧后的回调 */
} ProtoCtx;

/* Proto_FeedByte 的返回值 */
#define PROTO_PENDING           0    /* 还没收完，继续喂 */
#define PROTO_DONE              1    /* 收到一帧且 CRC 正确（已回调） */
#define PROTO_ERR_CRC          (-1)  /* 收完但 CRC 错，已丢弃并复位 */
#define PROTO_ERR_TOOLONG      (-2)  /* 长度字段超限，已丢弃并复位 */

/* ---------------- API ---------------- */

/* CRC16/IBM（Modbus 变体）：poly 0x8005 反射为 0xA001，init 0xFFFF，
   refin/refout = true，xorout = 0。标准自检：CRC16("123456789") = 0x4B37 */
uint16_t CRC16_Modbus(const uint8_t *data, uint32_t len);

/* 单字节累加（流式 CRC）。满足：Accumulate(CRC16(前n字节), 第n+1字节)
   == CRC16(前 n+1 字节)，所以可以收一个字节算一个字节 */
uint16_t CRC16_Accumulate(uint16_t crc, uint8_t b);

/* 复位解析器（换通道 / 链路重连 / 上电时调用） */
void     Proto_Init(ProtoCtx *ctx, ProtoCallback cb);

/* 逐字节喂入。返回 PROTO_DONE / PROTO_PENDING / 负错误码 */
int      Proto_FeedByte(ProtoCtx *ctx, uint8_t b);

/* 批量喂入（内部循环调 FeedByte，方便一次喂一整段 DMA 数据） */
void     Proto_FeedBytes(ProtoCtx *ctx, const uint8_t *buf, uint32_t len);

/* 周期调用：①更新内部时钟 ②处理"收到一半断了"的超时复位 */
void     Proto_Tick(ProtoCtx *ctx, uint32_t now_ms);

/* 打包一帧。out_buf 至少要 8+N 字节，返回整帧长度（失败返回 0） */
uint16_t Proto_BuildFrame(uint16_t cmd, const uint8_t *payload, uint16_t len,
                          uint8_t *out_buf, uint16_t out_cap);

/* 便捷：打包一条应答帧（数据区第 1 字节自动填错误码） */
uint16_t Proto_BuildAck(uint16_t req_cmd, uint8_t err_code,
                        const uint8_t *payload, uint16_t len,
                        uint8_t *out_buf, uint16_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* __PROTOCOL_H__ */
