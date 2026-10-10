/* 自定义协议：AA 55 | CMD(2B BE, bit15=1 为应答) | LEN(2B BE) | DATA(N) | CRC16(2B BE)
 * 整帧 8+N 字节，N<=256
 * CRC16/IBM：poly 0x8005（反射 0xA001），init 0xFFFF，右移，结果不取反
 * [约束] CRC 计算范围 = 命令字..数据末尾（4+N 字节，不含帧头）
 * 自检：CRC16("123456789") == 0x4B37
 */
/*
 * protocol.c —— mini-weather 自定义应用层协议实现
 * 纯 C89/C99 兼容，不依赖 HAL / RTOS，可直接在 PC 上编译验证
  */
#include "protocol.h"
#include "main.h"

/* 单步累加：喂一个字节，返回新的 CRC。CRC16_Modbus 就是它的循环版 */
uint16_t CRC16_Accumulate(uint16_t crc, uint8_t b)
{
    uint8_t bit;

    crc ^= (uint16_t)b;              /* ① 新字节并入余数的低 8 位 */
    for (bit = 0; bit < 8; bit++) {  /* ② 每位：右移，移出的 1 触发异或多项式 */
        if (crc & 0x0001u)
            crc = (uint16_t)((crc >> 1) ^ 0xA001u);
        else
            crc = (uint16_t)(crc >> 1);
    }
    return crc;
}

uint16_t CRC16_Modbus(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;   /* 初值必须是 0xFFFF，不是 0x0000 */
    uint32_t i;

    for (i = 0; i < len; i++) {
        crc = CRC16_Accumulate(crc, data[i]);
    }
    return crc;   /* refout 已体现在移位方向里，xorout=0 所以不处理 */
}

/* 二、打包 */

static void PutU16BE(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);      /* 高字节在前 */
    p[1] = (uint8_t)(v & 0xFF);
}

uint16_t Proto_BuildFrame(uint16_t cmd, const uint8_t *payload, uint16_t len,
                          uint8_t *out_buf, uint16_t out_cap)
{
    uint16_t crc;
    uint16_t total;
    uint16_t i;

    /* ① 参数合法性：长度超限 / 缓冲不够 / 空指针，一律返回 0 表示失败。
          注意 len==0 时 payload 允许为 NULL（空数据帧） */
    if (len > PROTO_MAX_PAYLOAD) return 0;
    if (out_buf == NULL) return 0;
    if (len > 0 && payload == NULL) return 0;
    total = (uint16_t)(PROTO_MIN_FRAME + len);   /* 8 + N */
    if (out_cap < total) return 0;

    /* ② 帧头 */
    out_buf[0] = PROTO_HEAD_HI;   /* 0xAA */
    out_buf[1] = PROTO_HEAD_LO;   /* 0x55 */

    /* ③ 命令字（大端） */
    PutU16BE(&out_buf[2], cmd);

    /* ④ 数据区长度 N（大端）。注意：只算数据区，
          不含帧头(2)/命令(2)/长度(2)/CRC(2) */
    PutU16BE(&out_buf[4], len);

    /* ⑤ 数据区 */
    for (i = 0; i < len; i++) out_buf[6 + i] = payload[i];

    /* ⑥ CRC16：从**命令字**开始（out_buf+2），算到数据区末尾，共 4+N 字节。
          帧头不参与：帧头错的话这帧早被当垃圾丢了，没必要让 CRC 再兜一次 */
    crc = CRC16_Modbus(&out_buf[2], (uint32_t)(4u + len));
    PutU16BE(&out_buf[6 + len], crc);

    return total;
}

/* 应答帧：命令字 = 请求命令 | 0x8000，数据区第 1 字节固定是错误码 */
uint16_t Proto_BuildAck(uint16_t req_cmd, uint8_t err_code,
                        const uint8_t *payload, uint16_t len,
                        uint8_t *out_buf, uint16_t out_cap)
{
    uint8_t  tmp[PROTO_MAX_PAYLOAD];
    uint16_t n = 0;
    uint16_t i;

    if (len > PROTO_MAX_PAYLOAD - 1) return 0;   /* 1 字节留给错误码 */

    tmp[n++] = err_code;                          /* 错误码占数据区第 1 字节 */
    for (i = 0; i < len; i++) tmp[n++] = payload[i];

    return Proto_BuildFrame((uint16_t)(req_cmd | PROTO_ACK_FLAG), tmp, n,
                            out_buf, out_cap);
}

/*
 * 三、解析：单字节喂入的状态机
 * 为什么必须是状态机：串口是**字节流**，没有消息边界。
 * 上位机发两帧，你可能一次性收到（粘包），也可能分 5 次收到（断帧）。
 * 状态机每次只吃 1 字节，天然无视这些切分方式。
  */

static void Proto_Reset(ProtoCtx *ctx)
{
    ctx->state    = P_ST_HEAD_HI;
    ctx->idx      = 0;
    ctx->cmd      = 0;
    ctx->len      = 0;
    ctx->crc_calc = 0xFFFFu;   /* ★ CRC 初值也要复位，否则下一帧从错误余数继续 */
    ctx->crc_recv = 0;
}

void Proto_Init(ProtoCtx *ctx, ProtoCallback cb)
{
    ctx->state       = P_ST_HEAD_HI;
    ctx->idx         = 0;
    ctx->cmd         = 0;
    ctx->len         = 0;
    ctx->crc_calc    = 0xFFFFu;
    ctx->crc_recv    = 0;
    ctx->last_ms     = 0;
    ctx->now_ms      = 0;
    ctx->rx_frames   = 0;
    ctx->err_crc     = 0;
    ctx->err_len     = 0;
    ctx->err_timeout = 0;
    ctx->cb          = cb;
}

int Proto_FeedByte(ProtoCtx *ctx, uint8_t b)
{
    if (ctx == NULL) return PROTO_PENDING;

    ctx->last_ms = ctx->now_ms;   /* 记录"刚收到字节"的时刻，供超时判断 */

    switch (ctx->state) {

    /* ---- 状态 0：等 0xAA ---- */
    case P_ST_HEAD_HI:
        if (b == PROTO_HEAD_HI) {
            ctx->state = P_ST_HEAD_LO;
        }
        /* 不是 0xAA 就一直待着 —— 相当于在字节流里"滑窗"找帧头 */
        break;

    /* ---- 状态 1：等 0x55 ---- */
    case P_ST_HEAD_LO:
        if (b == PROTO_HEAD_LO) {
            /* 帧头完整 → 开始收命令字，同时把所有"上一帧残留"清干净 */
            ctx->state    = P_ST_CMD;
            ctx->idx      = 0;
            ctx->cmd      = 0;
            ctx->len      = 0;
            ctx->crc_recv = 0;
            ctx->crc_calc = 0xFFFFu;   /* CRC 从头开始 */
        } else if (b == PROTO_HEAD_HI) {
            /* ★ 关键细节：连续两个 AA 时，第二个 AA 很可能是新帧头的开始。
               保持在本状态继续等 0x55，而不是退回 P_ST_HEAD_HI。
               否则 ".. AA | AA 55 .."（上帧尾巴 + 本帧头部）会被判成帧头错误 */
            /* stay in P_ST_HEAD_LO */
        } else {
            ctx->state = P_ST_HEAD_HI;
        }
        break;

    /* ---- 状态 2：收 2 字节命令字 ---- */
    case P_ST_CMD:
        ctx->cmd = (uint16_t)((ctx->cmd << 8) | b);   /* 大端：先来的是高字节 */
        ctx->crc_calc = CRC16_Accumulate(ctx->crc_calc, b);
        if (++ctx->idx >= 2) { ctx->idx = 0; ctx->state = P_ST_LEN; }
        break;

    /* ---- 状态 3：收 2 字节长度 ---- */
    case P_ST_LEN:
        ctx->len = (uint16_t)((ctx->len << 8) | b);
        ctx->crc_calc = CRC16_Accumulate(ctx->crc_calc, b);
        if (++ctx->idx >= 2) {
            ctx->idx = 0;
            /* ★ 长度合法性检查：超限立刻丢帧复位。
               没有这句的话，一个 0xFFFF 会让后面 65535 字节全被当数据吞掉，
               而且 buf[] 会越界写 —— 这是最容易被忽略的健壮性漏洞 */
            if (ctx->len > PROTO_MAX_PAYLOAD) {
                ctx->err_len++;
                Proto_Reset(ctx);
                return PROTO_ERR_TOOLONG;
            }
            ctx->state = (ctx->len == 0) ? P_ST_CRC : P_ST_DATA;
        }
        break;

    /* ---- 状态 4：收 N 字节数据 ---- */
    case P_ST_DATA:
        ctx->buf[ctx->idx] = b;
        ctx->crc_calc = CRC16_Accumulate(ctx->crc_calc, b);
        if (++ctx->idx >= ctx->len) { ctx->idx = 0; ctx->state = P_ST_CRC; }
        break;

    /* ---- 状态 5：收 2 字节 CRC ---- */
    case P_ST_CRC:
        ctx->crc_recv = (uint16_t)((ctx->crc_recv << 8) | b);
        if (++ctx->idx >= 2) {
            ctx->idx = 0;
            /* ★ 大端的好处：先收的是高字节，拼出来的 crc_recv 就是"高在前"的值，
               可以直接和 crc_calc 比，不用交换字节 */
            if (ctx->crc_recv == ctx->crc_calc) {
                ctx->rx_frames++;
                ctx->state = P_ST_HEAD_HI;   /* ★ 收完立刻回等帧头，准备接下一帧 */
                if (ctx->cb != NULL) ctx->cb(ctx->cmd, ctx->buf, ctx->len);
                return PROTO_DONE;
            } else {
                ctx->err_crc++;
                Proto_Reset(ctx);            /* CRC 错：整帧丢弃 */
                return PROTO_ERR_CRC;
            }
        }
        break;

    default:
        Proto_Reset(ctx);
        break;
    }
    return PROTO_PENDING;
}

void Proto_FeedBytes(ProtoCtx *ctx, const uint8_t *buf, uint32_t len)
{
    uint32_t i;
    if (ctx == NULL || buf == NULL) return;
    for (i = 0; i < len; i++) (void)Proto_FeedByte(ctx, buf[i]);
}

void Proto_Tick(ProtoCtx *ctx, uint32_t now_ms)
{
    if (ctx == NULL) return;

    ctx->now_ms = now_ms;   /* ① 更新内部时钟（由外部周期性灌入） */

    /* ② 只有"已经开始收但还没收完"才需要超时保护。
          没有这个保护的话：一帧收到一半对端断电，状态机会永远卡在 P_ST_DATA，
          后面所有正确帧全被吞掉 —— 表现为"通信死掉，重启才好" */
    if (ctx->state != P_ST_HEAD_HI) {
        if ((uint32_t)(now_ms - ctx->last_ms) >= PROTO_RX_TIMEOUT_MS) {
            ctx->err_timeout++;
            Proto_Reset(ctx);
        }
    }
}
