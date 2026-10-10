/* 上位机协议层：组帧 / CRC16 / 固件加载（bin 与 Intel HEX 均支持）
 * [约束] Intel HEX 必须解析地址记录与校验和；解析结果须与 fromelf 产出的 .bin 逐字节一致。
 * [约束] OTA 只发裸 .bin；发 .hex 会导致长度超限与 CRC32 不符。
 */
#include "protocol.h"
#include <QFile>        /* loadFirmware 要读文件 */
#include <QList>        /* QByteArray::split 的返回值是 QList<QByteArray> */


namespace Proto {

quint16 crc16Accum(quint16 crc, quint8 b)
{
    crc ^= b;                        /* ① 输入字节异或进 CRC 低 8 位 */
    for (int i = 0; i < 8; i++) {    /* ② 一位一位右移 8 次 */
        if (crc & 0x0001u)           /*    移出去的那位是 1 → 异或多项式 */
            crc = (quint16)((crc >> 1) ^ 0xA001u);
        else                         /*    是 0 → 直接右移 */
            crc = (quint16)(crc >> 1);
    }
    return crc;
}

quint16 crc16(const quint8 *d, quint32 len)
{
    quint16 crc = 0xFFFFu;           /* 初值 */
    for (quint32 i = 0; i < len; i++)
        crc = crc16Accum(crc, d[i]);
    return crc;                      /* 不取反，直接就是结果 */
}

/*
 * CRC32/IEEE 802.3
 * 用查表法：256 个表项只算一次，之后每字节只做一次查表 + 异或，比逐位移位快 8 倍。
 * 表是"反射多项式" 0xEDB88320 生成的。
 * 自检：CRC32("123456789") == 0xCBF43926
  */
static quint32 s_crc32Table[256];
static bool    s_crc32TableReady = false;

/* C++ 里的"静态初始化"技巧：
   用一个全局对象的构造函数来建表，保证 main() 之前表就准备好了。
   （如果你不熟悉，就当它是"程序启动时自动执行一次建表"） */
static struct Crc32TableInit {
    Crc32TableInit() {
        for (quint32 i = 0; i < 256; i++) {
            quint32 c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            s_crc32Table[i] = c;
        }
        s_crc32TableReady = true;
    }
} s_crc32TableInit;

quint32 crc32(const quint8 *d, quint32 len)
{
    (void)s_crc32TableReady;         /* 确保建表代码不被编译器优化掉 */
    quint32 c = 0xFFFFFFFFu;         /* 初值全 1 */
    for (quint32 i = 0; i < len; i++)
        c = s_crc32Table[(c ^ d[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;          /* 结果取反（这是协议规定） */
}

/* 打包一帧 */
QByteArray buildFrame(quint16 cmd, const quint8 *payload, quint16 len)
{
    QByteArray f;
    /* 预留空间，避免 QByteArray 反复扩容（小优化，但养成习惯没坏处） */
    f.reserve(8 + len);

    /* ① 帧头 2 字节 */
    f.append(char(PROTO_HEAD_HI));
    f.append(char(PROTO_HEAD_LO));

    /* ② 命令字 2 字节，大端 */
    putU16BE(f, cmd);

    /* ③ 长度 2 字节，大端 */
    putU16BE(f, len);

    /* ④ 数据区 */
    if (len > 0 && payload != nullptr)
        f.append(reinterpret_cast<const char *>(payload), len);

    /* ⑤ CRC16：★ 计算范围是 [命令字 .. 数据末尾]，不含帧头！
       也就是从第 2 字节开始，共 4 + len 个字节。
       这是初学者最容易错的地方 —— 把帧头算进去，
       下位机会算出另一个值，表现为"每帧都 CRC 错"。 */
    quint16 crc = crc16(reinterpret_cast<const quint8 *>(f.constData()) + 2,
                        quint32(4 + len));
    putU16BE(f, crc);

    return f;
}

QByteArray buildFrame(quint16 cmd, const QByteArray &payload)
{
    return buildFrame(cmd,
                      reinterpret_cast<const quint8 *>(payload.constData()),
                      quint16(payload.size()));
}

/* 大端读写工具 */
void putU16BE(QByteArray &b, quint16 v)
{
    b.append(char((v >> 8) & 0xFF));   /* 先放高字节 */
    b.append(char(v & 0xFF));          /* 再放低字节 */
}

void putU32BE(QByteArray &b, quint32 v)
{
    b.append(char((v >> 24) & 0xFF));  /* 从最高字节开始放，4 次 */
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 8)  & 0xFF));
    b.append(char(v & 0xFF));
}

quint16 getU16BE(const QByteArray &b, int i)
{
    /* quint8 强转是必须的：
       char 在多数平台是"有符号"的，0x80 以上会变成负数，
       左移后高位全是 1，结果就错了。这是 C/C++ 的经典坑。 */
    return quint16((quint8(b.at(i)) << 8) | quint8(b.at(i + 1)));
}

quint32 getU32BE(const QByteArray &b, int i)
{
    return (quint32(quint8(b.at(i))     << 24) |
            quint32(quint8(b.at(i + 1)) << 16) |
            quint32(quint8(b.at(i + 2)) << 8)  |
            quint32(quint8(b.at(i + 3))));
}

QString toHexString(const QByteArray &b)
{
    QString s;
    s.reserve(b.size() * 3);
    for (int i = 0; i < b.size(); i++) {
        if (i) s += QLatin1Char(' ');
        /* %1 是占位符，arg(值, 宽度, 进制, 填充字符) */
        s += QString("%1").arg(quint32(quint8(b.at(i))), 2, 16, QLatin1Char('0'));
    }
    return s.toUpper();
}


bool parseIntelHex(const QByteArray &txt, QByteArray *out, quint32 *base, QString *err)
{
    quint32 extBase  = 0;      /* 类型 02/04 记录给出的地址高位 */
    quint32 minAddr  = 0;      /* 第一个数据字节的绝对地址 */
    quint32 nextAddr = 0;      /* 下一个数据字节应落的地址（用来查空洞/重叠） */
    bool    started  = false;
    bool    ended    = false;

    out->clear();

    /* 逐行解析；split 出来是 QList<QByteArray>，CR/LF 交给 trim 处理 */
    const QList<QByteArray> lines = txt.split('\n');
    for (int ln = 0; ln < lines.size() && !ended; ++ln) {
        const QByteArray s = lines.at(ln).trimmed();
        if (s.isEmpty()) continue;

        /* 最短的合法行是末尾的 ":00000001FF"，共 11 个字符 */
        if (s.size() < 11 || s.at(0) != ':') {
            *err = QString("第 %1 行格式非法（缺 ':' 或长度不足）").arg(ln + 1);
            return false;
        }
        const QByteArray bin = QByteArray::fromHex(s.mid(1));   /* ASCII → 字节 */
        if (bin.size() < 5) {
            *err = QString("第 %1 行含非法十六进制字符").arg(ln + 1);
            return false;
        }

        const int     cnt = quint8(bin.at(0));                  /* 本行数据长度 */
        if (bin.size() != cnt + 5) {                            /* 1+2+1+cnt+1 */
            *err = QString("第 %1 行长度与声明不符").arg(ln + 1);
            return false;
        }
        const quint16 off  = quint16((quint8(bin.at(1)) << 8) | quint8(bin.at(2)));
        const quint8  type = quint8(bin.at(3));

        /* 校验和：除 ':' 外的所有字节相加，低 8 位必须为 0 */
        quint8 sum = 0;
        for (int i = 0; i < bin.size(); ++i) sum = quint8(sum + quint8(bin.at(i)));
        if (sum != 0) {
            *err = QString("第 %1 行校验和错误").arg(ln + 1);
            return false;
        }

        if (type == 0x00) {                                     /* --- 数据记录 --- */
            const quint32 addr = extBase + off;
            if (!started) { minAddr = addr; nextAddr = addr; started = true; }
            if (addr < nextAddr) {          /* 地址回退/重叠：拼出来的镜像必然错 */
                *err = QString("第 %1 行地址回退（0x%2 < 0x%3），文件非顺序排布")
                           .arg(ln + 1)
                           .arg(addr, 8, 16, QLatin1Char('0'))
                           .arg(nextAddr, 8, 16, QLatin1Char('0')).toUpper();
                return false;
            }
            /* 中间有空洞（多段代码）就补 0xFF —— Flash 擦除后的值，
               和后端"未写入的字节读出来是 0xFF"保持一致 */
            while (nextAddr < addr) { out->append(char(0xFF)); ++nextAddr; }
            out->append(bin.constData() + 4, cnt);
            nextAddr += quint32(cnt);
        } else if (type == 0x01) {                              /* --- 文件结束 --- */
            ended = true;
        } else if (type == 0x02) {          /* 扩展段地址（x86 段 <<4），ARM 极少见 */
            extBase = quint32((quint8(bin.at(4)) << 8) | quint8(bin.at(5))) << 4;
        } else if (type == 0x04) {          /* 扩展线性地址（<<16）—— 本工程用的这个 */
            extBase = quint32((quint8(bin.at(4)) << 8) | quint8(bin.at(5))) << 16;
        }                                   /* 03/05 是入口地址记录，无数据，忽略 */
    }

    if (!started || out->isEmpty()) { *err = "HEX 里没有数据记录"; return false; }
    *base = minAddr;
    return true;
}

bool loadFirmware(const QString &path, QByteArray *out, quint32 *base, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { *err = "打不开文件：" + f.errorString(); return false; }
    QByteArray raw = f.readAll();
    f.close();
    if (raw.isEmpty()) { *err = "文件是空的"; return false; }

    /* 去掉可能存在的 UTF-8 BOM，否则"以 ':' 开头"的判断会失手 */
    if (raw.startsWith("\xEF\xBB\xBF")) raw.remove(0, 3);

    if (raw.startsWith(':')) return parseIntelHex(raw, out, base, err);

    /* 不是 ':' 开头 → 当成裸二进制。bin 里没有地址信息，只能填 0；
       这正是"bin 必须以 APP_ADDR 为链接基址"只能靠人工保证的原因。 */
    *out  = raw;
    *base = 0;
    return true;
}

} // namespace Proto


/* FrameParser —— 接收状态机 */
FrameParser::FrameParser(QObject *parent)
    : QObject(parent)
{
    reset();
    m_frames = 0;
    m_crcErr = 0;
}

/* 回到初始态。
   注意：统计计数不清零 —— 它们是给"这一路链路总共怎么样"用的。 */
void FrameParser::reset()
{
    m_state    = StHeadHi;
    m_idx      = 0;
    m_cmd      = 0;
    m_len      = 0;
    m_buf.clear();
    m_crcCalc  = 0xFFFFu;            /* ★ CRC 初值，每次收新帧都要重新置位 */
    m_crcRecv  = 0;
    m_lastByte.invalidate();         /* 计时器失效 = 还没开始收 */
}

void FrameParser::toHeadHi()
{
    m_state   = StHeadHi;
    m_idx     = 0;
    m_buf.clear();
    m_crcCalc = 0xFFFFu;             /* ★ 关键：CRC 初值必须复位，否则下一帧必错 */
    m_lastByte.invalidate();
}

void FrameParser::feed(const QByteArray &d)
{
    for (int i = 0; i < d.size(); i++)
        feedByte(quint8(d.at(i)));
}

void FrameParser::feedByte(quint8 b)
{
    /* --- 健壮性①：超时复位 -------------------------------------------------
       场景：收到 "AA 55 00" 后链路断了，状态机永远卡在"收命令字"。
       处理：只要离上个字节超过 200ms，不管在哪个状态，一律丢掉重来。
       没有这条，一次干扰就能让上位机"再也收不到数据"（最常见的假死）。 */
    if (m_state != StHeadHi && m_lastByte.isValid()
        && m_lastByte.elapsed() > PROTO_RX_TIMEOUT_MS) {
        toHeadHi();
    }

    switch (m_state) {

    case StHeadHi:
        if (b == PROTO_HEAD_HI) {
            m_state = StHeadLo;
            m_lastByte.restart();
        }
        break;

    case StHeadLo:
        if (b == PROTO_HEAD_LO) {
            m_state   = StCmd;
            m_idx     = 0;
            m_crcCalc = 0xFFFFu;     /* 开始算 CRC 了，先把初值放好 */
            m_lastByte.restart();
        } else if (b == PROTO_HEAD_HI) {
            /* "AA AA" 的第二个 AA：它可能是真正的帧头开头，留在当前态 */
        } else {
            toHeadHi();
        }
        break;

    /* --- 收 2 字节命令字（大端）--- */
    case StCmd:
        if (m_idx == 0) m_cmd = quint16(quint16(b) << 8);  /* 高字节 */
        else            m_cmd = quint16(m_cmd | b);        /* 低字节 */
        m_crcCalc = Proto::crc16Accum(m_crcCalc, b);       /* 边收边算 */
        if (++m_idx >= 2) { m_state = StLen; m_idx = 0; }
        m_lastByte.restart();
        break;

    /* --- 收 2 字节长度 ---
       顺便做"长度上限"检查：下位机缓冲区只有 256 字节，
       收到一个 60000 的长度，如果不拦，memcpy 就会越界写爆内存。 */
    case StLen:
        if (m_idx == 0) m_len = quint16(quint16(b) << 8);
        else            m_len = quint16(m_len | b);
        m_crcCalc = Proto::crc16Accum(m_crcCalc, b);
        if (++m_idx >= 2) {
            if (m_len > PROTO_MAX_PAYLOAD) {   /* ★ 健壮性②：长度上限 */
                toHeadHi();
                break;
            }
            m_buf.clear();
            if (m_len == 0) { m_state = StCrc; }   /* 空数据帧直接去收 CRC */
            else            { m_state = StData; }
            m_idx = 0;
        }
        m_lastByte.restart();
        break;

    /* --- 收 N 字节数据 --- */
    case StData:
        m_buf.append(char(b));
        m_crcCalc = Proto::crc16Accum(m_crcCalc, b);
        if (++m_idx >= m_len) { m_state = StCrc; m_idx = 0; }
        m_lastByte.restart();
        break;

    /* --- 收 2 字节 CRC 并判决 --- */
    case StCrc:
        if (m_idx == 0) m_crcRecv = quint16(quint16(b) << 8);
        else            m_crcRecv = quint16(m_crcRecv | b);
        if (++m_idx >= 2) {
            /* ★ 收满一帧：无论对错都必须回到等帧头。
               忘了复位 = 只能收到第一帧，之后全丢（新手第二大坑） */
            if (m_crcRecv == m_crcCalc) {
                m_frames++;
                emit frameReady(m_cmd, m_buf);
            } else {
                m_crcErr++;
                emit crcError();
            }
            toHeadHi();
        }
        m_lastByte.restart();
        break;
    }
}
