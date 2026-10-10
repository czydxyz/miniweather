/* 帧格式：AA 55 | CMD(2B BE, bit15=1 应答) | LEN(2B BE) | DATA(N) | CRC16(2B BE)
 * CRC16/IBM：poly 0x8005（反射 0xA001），init 0xFFFF，右移，结果不取反
 * [约束] CRC 范围 = 命令字..数据末尾（不含帧头）；自检 CRC16("123456789") == 0x4B37
 * [约束] 固件按内容识别：以 ':' 开头按 Intel HEX 解析，否则按裸 bin。
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>

/* ---------------- 常量：必须与下位机 protocol.h 完全一致 ---------------- */

#define PROTO_HEAD_HI       0xAAu   /* 帧头第 1 字节（先发） */
#define PROTO_HEAD_LO       0x55u   /* 帧头第 2 字节 */
#define PROTO_ACK_FLAG      0x8000u /* 命令字 bit15=1 → 这是应答帧 */
#define PROTO_MAX_PAYLOAD   256u    /* 数据区上限，和下位机一致 */
#define PROTO_RX_TIMEOUT_MS 200u    /* 收一半断了的超时复位时间 */

/* 命令字（与下位机 CMD_* 对应） */
enum Cmd : quint16 {
    CMD_GET_VERSION   = 0x0001, /* 查询固件版本：请求空，应答含版本串 */
    CMD_HEARTBEAT     = 0x0002, /* 心跳：应答含运行秒数 + OTA 状态 */
    CMD_UPDATE_START  = 0x0003, /* 开始升级：总大小(4)+总包数(2)+CRC32(4)+Bank(1) */
    CMD_UPDATE_BLOCK  = 0x0004, /* 固件块：偏移(4)+数据(<=128) */
    CMD_UPDATE_FINISH = 0x0005, /* 升级完成：整包 CRC32(4) */
    CMD_SWITCH_BANK   = 0x0006, /* 切换 Bank：Bank 号(1) */
    CMD_READ_SENSOR   = 0x0007, /* 读传感器：应答 13 字节 */
    CMD_CTRL_ACTUATOR = 0x0008  /* 控制执行器：设备ID(1)+动作(1) */
};

/* 错误码（应答帧数据区第 1 字节） */
enum Err : quint8 {
    ERR_OK      = 0x00, /* 成功 */
    ERR_CMD     = 0x01, /* 未知命令 */
    ERR_PARAM   = 0x02, /* 参数非法 */
    ERR_LEN     = 0x03, /* 长度超限 */
    ERR_BUSY    = 0x04, /* 设备忙（比如没 Start 就发块） */
    ERR_FLASH   = 0x05, /* Flash 擦写失败 */
    ERR_RANGE   = 0x06, /* 偏移越界 / 大小超限 */
    ERR_TIMEOUT = 0x07, /* 超时 */
    ERR_CRC32   = 0x08  /* 整包 CRC32 不匹配 */
};

/* 下位机 OTA 状态机状态（心跳应答会带回它，OTA 时靠它判断"擦完了没"） */
enum OtaState : quint8 {
    OTA_IDLE      = 0, /* 空闲 */
    OTA_ERASING   = 1, /* 正在按扇区擦除（要等！） */
    OTA_READY     = 2, /* 擦完了，可以发固件块了 */
    OTA_RECEIVING = 3, /* 正在收块 */
    OTA_DONE      = 4, /* 收完且 CRC32 通过 */
    OTA_FAIL      = 5  /* 失败 */
};

/* 设备 ID（CMD_CTRL_ACTUATOR 的第 1 个参数） */
enum Device : quint8 {
    DEV_SERVO = 0x01, /* 舵机 / 开窗机构 */
    DEV_RELAY = 0x02, /* 继电器 / 风扇   */
    DEV_BEEP  = 0x03  /* 蜂鸣器          */
};

/* OTA 相关常量（对应下位机 comm.h 里的 OTA_BLOCK_DATA_MAX / OTA_BANK_SIZE）
   ★ 单块数据上限 128 字节：下位机收到 >128 会直接回 ERR_PARAM。
     这个值同时受协议限制：数据区 = 4 字节偏移 + 128 数据 = 132 ≤ 256。
     Bank 容量 256KB：固件超过这个大小，下位机会回 ERR_RANGE。 */
#define OTA_BLOCK_DATA  128
#define OTA_BANK_SIZE   (256 * 1024)

#define OTA_APP_BASE    0x08010000u

namespace Proto {

quint16 crc16Accum(quint16 crc, quint8 b);        /* 流式：喂一个字节 */
quint16 crc16(const quint8 *d, quint32 len);      /* 一次性算一段 */

/* ---------------- CRC32/IEEE 802.3 ----------------
   poly 0xEDB88320（反射形式），初值 0xFFFFFFFF，结果异或 0xFFFFFFFF
   标准自检：CRC32("123456789") == 0xCBF43926
   用途：整个固件 bin 的校验和，START 时告诉下位机，FINISH 时双方对账 */
quint32 crc32(const quint8 *d, quint32 len);

/* ---------------- 打包 ----------------
   把 cmd + payload 组装成完整的一帧（含帧头和 CRC16），返回可直接发送的字节串 */
QByteArray buildFrame(quint16 cmd, const quint8 *payload, quint16 len);

/* 重载：直接吃 QByteArray，省得调用方到处 .constData() */
QByteArray buildFrame(quint16 cmd, const QByteArray &payload);

/* ---------------- 大端读写小工具 ----------------
   协议里所有多字节字段都是"高字节在前"。
   x86 是小端机，直接 memcpy 会把 0x1234 存成 34 12，所以必须手动移位。 */
void     putU16BE(QByteArray &b, quint16 v);
void     putU32BE(QByteArray &b, quint32 v);
quint16  getU16BE(const QByteArray &b, int i);
quint32  getU32BE(const QByteArray &b, int i);

/* 把字节串转成 "AA 55 00 01 ..." 这样的调试文本 */
QString  toHexString(const QByteArray &b);

bool loadFirmware(const QString &path, QByteArray *out, quint32 *base, QString *err);

/* 只解析 Intel HEX 文本（不碰文件系统），单独暴露出来便于单元测试 */
bool parseIntelHex(const QByteArray &txt, QByteArray *out, quint32 *base, QString *err);

} // namespace Proto


class FrameParser : public QObject
{
    Q_OBJECT
public:
    explicit FrameParser(QObject *parent = nullptr);

    void reset();                    /* 复位到"等 0xAA"（换通道、重连时调用） */
    void feed(const QByteArray &d);  /* 批量喂字节，内部逐字节喂给 feedByte */

    /* 统计：出问题先看这几个数，和下位机 ProtoCtx 里的统计是一套思路 */
    quint32 frames() const    { return m_frames; }
    quint32 crcErrors() const { return m_crcErr; }

signals:
    /* 收到一帧且 CRC 正确才发射。payload 是纯数据区（不含帧头/命令/长度/CRC） */
    void frameReady(quint16 cmd, const QByteArray &payload);
    void crcError();                 /* CRC 错了（只用于日志提示） */

private:
    /* 六个状态，和下位机 ProtoState 一一对应 */
    enum State { StHeadHi, StHeadLo, StCmd, StLen, StData, StCrc };

    void    feedByte(quint8 b);      /* 单字节喂入，状态迁移都在这里 */
    void    toHeadHi();              /* 回到"等 0xAA"并清干净临时变量 */

    State       m_state;             /* 当前状态 */
    int         m_idx;               /* 当前字段已收字节数 */
    quint16     m_cmd;               /* 正在收的命令字 */
    quint16     m_len;               /* 数据区长度 N */
    QByteArray  m_buf;               /* 数据区缓冲 */
    quint16     m_crcCalc;           /* 边收边算的 CRC（命令+长度+数据） */
    quint16     m_crcRecv;           /* 收到的 CRC */
    QElapsedTimer m_lastByte;        /* 最后一次收到字节的时刻（超时复位用） */
    quint32     m_frames;            /* 成功帧计数 */
    quint32     m_crcErr;            /* CRC 错计数 */
};

#endif // PROTOCOL_H
