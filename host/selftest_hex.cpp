#include <QCoreApplication>
#include <QByteArray>
#include <QString>
#include <QFile>
#include <cstdio>

#include "protocol.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char *what)
{
    if (cond) { ++g_pass; std::printf("  [PASS] %s\n", what); }
    else      { ++g_fail; std::printf("  [FAIL] %s\n", what); }
}

/* 直接读裸文件（测试里用作"标准答案"，不走被测代码） */
static QByteArray rawRead(const QString &p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    QByteArray d = f.readAll();
    f.close();
    return d;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Q_UNUSED(app);

    const QString hexPath = QString::fromUtf8("D:/mini weather/miniweather/MDK-ARM/miniweather/miniweather.hex");
    const QString binPath = QString::fromUtf8("D:/mini weather/host/App.bin");

    std::printf("=========== loadFirmware 回归测试 ===========\n\n");

    /* ---------- 1. 标准答案（fromelf 产出的裸 bin） ---------- */
    const QByteArray ref = rawRead(binPath);
    std::printf("[基准] App.bin: %d 字节  CRC32=0x%08X\n",
                int(ref.size()),
                Proto::crc32(reinterpret_cast<const quint8 *>(ref.constData()), quint32(ref.size())));
    check(!ref.isEmpty(), "基准文件 App.bin 可读");

    /* ---------- 2. 解析 .hex ---------- */
    QByteArray fw;
    quint32    base = 0;
    QString    err;
    const bool ok = Proto::loadFirmware(hexPath, &fw, &base, &err);
    check(ok, "loadFirmware 解析 .hex 成功");
    if (!ok) {
        std::printf("       失败原因: %s\n", err.toUtf8().constData());
        std::printf("\n结果: %d 通过 / %d 失败\n", g_pass, g_fail);
        return 1;
    }

    const quint32 crcHex = Proto::crc32(reinterpret_cast<const quint8 *>(fw.constData()),
                                        quint32(fw.size()));
    std::printf("[解析] hex -> %d 字节  基址=0x%08X  CRC32=0x%08X\n\n",
                int(fw.size()), base, crcHex);

    /* ---------- 3. 核心断言：解析结果 == 裸 bin ---------- */
    check(base == 0x08010000u, "解析出的起始地址 = 0x08010000（APP_ADDR）");
    check(fw.size() == ref.size(), "长度与 App.bin 一致");
    check(fw == ref, "内容与 App.bin 逐字节完全一致");
    check(crcHex == Proto::crc32(reinterpret_cast<const quint8 *>(ref.constData()),
                                 quint32(ref.size())),
          "CRC32 与 App.bin 一致（下位机 FINISH 对账用的就是这个值）");

    /* ---------- 4. 对照：直接当 bin 读会有多离谱 ---------- */
    const QByteArray naive = rawRead(hexPath);
    std::printf("\n[对照] 不做解析、直接 readAll(hex): %d 字节, CRC32=0x%08X\n",
                int(naive.size()),
                Proto::crc32(reinterpret_cast<const quint8 *>(naive.constData()), quint32(naive.size())));
    std::printf("       虚高 %.2f 倍，且 %d > Bank 上限 %d → 必然 ERR_RANGE\n\n",
                double(naive.size()) / double(ref.size() ? ref.size() : 1),
                int(naive.size()), int(OTA_BANK_SIZE));
    check(naive.size() != ref.size(), "确认\"不解析直接用\"得到的是错误镜像（这正是之前的 bug）");

    /* ---------- 5. 坏输入必须被拒绝 ---------- */
    {
        /* 5.1 校验和错：把文件结束记录 ":00000001FF" 的校验和 FF 改成 00。
               长度不变（11 字符）、cnt=0 也合法，只有校验和对不上 →
               应当精确命中"校验和错误"这条分支 */
        QByteArray bad = naive;
        bad.replace(":00000001FF", ":0000000100");
        QByteArray o; quint32 b = 0; QString e;
        check(!Proto::parseIntelHex(bad, &o, &b, &e), "校验和错误 → 拒绝");
        std::printf("       原因: %s\n", e.toUtf8().constData());
    }
    {
        /* 5.2 中途截断：数据长度声称 16 字节，实际只给了 8 字节 */
        const QByteArray bad(":1000000090D300205D020108\r\n");
        QByteArray o; quint32 b = 0; QString e;
        check(!Proto::parseIntelHex(bad, &o, &b, &e), "记录长度不符 → 拒绝");
        std::printf("       原因: %s\n", e.toUtf8().constData());
    }
    {
        /* 5.3 非 hex 文本（不是 ':' 开头）走不到 parseIntelHex，
               loadFirmware 会当 bin 处理 —— 这里直接喂给 parseIntelHex 验证它报警 */
        const QByteArray bad("hello world, not a hex file\n");
        QByteArray o; quint32 b = 0; QString e;
        check(!Proto::parseIntelHex(bad, &o, &b, &e), "非 HEX 文本 → 拒绝");
        std::printf("       原因: %s\n", e.toUtf8().constData());
    }
    {
        /* 5.4 空文件 */
        QByteArray o; quint32 b = 0; QString e;
        check(!Proto::loadFirmware(QString::fromUtf8("D:/mw_build/__not_exist__.bin"), &o, &b, &e),
              "文件不存在 → 拒绝");
    }

    std::printf("\n=========== 结果: %d 通过 / %d 失败 ===========\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
