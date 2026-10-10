/* 临时自检程序：验证上位机协议层与下位机算法一致 */
#include "protocol.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
static void check(const char *name, quint32 got, quint32 expect)
{
    bool ok = (got == expect);
    printf("%-28s got=0x%08X expect=0x%08X  %s\n",
           name, got, expect, ok ? "OK" : "*** FAIL ***");
    if (!ok) g_fail++;
}

int main()
{
    const char *s = "123456789";

    /* ① CRC16 标准自检值（与下位机 protocol.c 注释一致） */
    check("CRC16(\"123456789\")",
          Proto::crc16((const quint8 *)s, 9), 0x4B37);

    /* ② CRC32 标准自检值 */
    check("CRC32(\"123456789\")",
          Proto::crc32((const quint8 *)s, 9), 0xCBF43926);

    /* ③ 打包空数据帧 CMD_GET_VERSION：
          期望与《第9节》里手工算的联调帧 AA 55 00 01 00 00 E4 51 完全一致 */
    QByteArray f = Proto::buildFrame(CMD_GET_VERSION, QByteArray());
    printf("buildFrame(0x0001) = %s\n", qPrintable(Proto::toHexString(f)));
    check("frame size", quint32(f.size()), 8);
    check("head", quint32(quint8(f.at(0)) << 8 | quint8(f.at(1))), 0xAA55);
    check("crc16 hi/lo", quint32(quint8(f.at(6)) << 8 | quint8(f.at(7))), 0xE451);

    /* ④ 往返测试：打包 → 喂给状态机 → 应还原出同样的命令和数据 */
    QByteArray payload;
    for (int i = 0; i < 40; i++) payload.append(char(i * 7));

    QByteArray frame = Proto::buildFrame(CMD_UPDATE_BLOCK, payload);

    bool got = false;
    quint16 gotCmd = 0;
    QByteArray gotData;
    FrameParser parser;
    QObject::connect(&parser, &FrameParser::frameReady,
                     [&](quint16 c, const QByteArray &p) {
                         got = true; gotCmd = c; gotData = p;
                     });
    parser.feed(frame);
    check("roundtrip got", quint32(got), 1);
    check("roundtrip cmd", gotCmd, quint32(CMD_UPDATE_BLOCK));
    check("roundtrip len", quint32(gotData.size()), quint32(payload.size()));
    check("roundtrip data equal", quint32(gotData == payload), 1);

    /* ⑤ 拆包测试：一帧分 3 次喂（模拟串口一次来几个字节），仍应解出 */
    got = false;
    parser.reset();
    parser.feed(frame.mid(0, 3));
    parser.feed(frame.mid(3, 20));
    parser.feed(frame.mid(23));
    check("split-feed got", quint32(got), 1);
    check("split-feed cmd", gotCmd, quint32(CMD_UPDATE_BLOCK));

    /* ⑥ 干扰测试：先喂 5 个垃圾字节再喂正常帧，应能自动同步上 */
    got = false;
    parser.reset();
    QByteArray junk;
    junk.append(char(0x00)); junk.append(char(0xFF));
    junk.append(char(0xAA)); junk.append(char(0xAA)); junk.append(char(0x12));
    parser.feed(junk);
    parser.feed(frame);
    check("after-junk got", quint32(got), 1);

    /* ⑦ CRC 错误检测：把数据区改一个字节，应被判为 CRC 错（收不到帧） */
    got = false;
    parser.reset();
    QByteArray bad = frame;
    bad[10] = char(quint8(bad.at(10)) ^ 0xFF);
    parser.feed(bad);
    check("corrupted rejected", quint32(got), 0);
    check("crcErr counter", parser.crcErrors(), 1);

    printf("\n%s\n", g_fail == 0 ? "==== ALL PASS ====" : "==== FAILED ====");
    return g_fail;
}
