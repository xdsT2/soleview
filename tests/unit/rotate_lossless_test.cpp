// RotateLossless 纯字节无损写回单元测试（#6）：D4 合成表闭环、格式拦截、
// JPEG 方向标签原地改 / 补写、压缩扫描区逐字节不变、原子写回失败干净回退。
#include "RotateLossless.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

static int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            ++g_failures; \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static QString sha256(const QByteArray &d)
{
    return QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex());
}

// 取出 JPEG 熵扫描区（SOS 载荷起到 EOI 前）；解析失败返回空。
static QByteArray jpegScan(const QByteArray &data)
{
    if (data.size() < 4 || quint8(data[0]) != 0xFF || quint8(data[1]) != 0xD8)
        return {};
    int p = 2;
    while (p + 4 <= data.size()) {
        if (quint8(data[p]) != 0xFF)
            return {};
        const quint8 m = quint8(data[p + 1]);
        if (m == 0xDA) {
            const int ln = quint8(data[p + 2]) * 256 + quint8(data[p + 3]);
            return data.mid(p + 2 + ln, data.size() - p - 2 - ln - 2);
        }
        if (m == 0xD9)
            return {};
        const int ln = quint8(data[p + 2]) * 256 + quint8(data[p + 3]);
        if (ln < 2 || p + 2 + ln > data.size())
            return {};
        p += 2 + ln;
    }
    return {};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // 1. 合成表闭环：顺时针四次回到原值；顺 + 逆抵消；两次镜像抵消。
    for (int o = 1; o <= 8; ++o) {
        int t = o;
        for (int i = 0; i < 4; ++i)
            t = RotateLossless::composeRotateCw(t);
        CHECK(t == o);
        t = RotateLossless::composeRotateCcw(RotateLossless::composeRotateCw(o));
        CHECK(t == o);
        CHECK(RotateLossless::composeFlipH(RotateLossless::composeFlipH(o)) == o);
        CHECK(RotateLossless::composeFlipV(RotateLossless::composeFlipV(o)) == o);
    }
    // 实证锚点（Pillow Transpose ground truth + UI 探针校准）：应用内右转 = 屏幕顺时针 90°。
    CHECK(RotateLossless::composeRotateCw(1) == 6);
    CHECK(RotateLossless::composeRotateCw(6) == 3);
    CHECK(RotateLossless::composeRotateCw(3) == 8);
    CHECK(RotateLossless::composeRotateCw(8) == 1);
    CHECK(RotateLossless::composeRotateCcw(1) == 8);
    CHECK(RotateLossless::composeRotate180(1) == 3);
    CHECK(RotateLossless::composeRotate180(6) == 8);
    CHECK(RotateLossless::composeFlipH(1) == 2);
    CHECK(RotateLossless::composeFlipV(1) == 4);

    // 2. 不支持格式拦截（按后缀 / 结构口径）。
    CHECK(!RotateLossless::probe(QStringLiteral("x.png")).supported);
    CHECK(RotateLossless::probe(QStringLiteral("x.png")).reason == QStringLiteral("PNG"));
    CHECK(RotateLossless::probe(QStringLiteral("x.bmp")).reason == QStringLiteral("BMP"));
    CHECK(RotateLossless::probe(QStringLiteral("x.gif")).reason == QStringLiteral("GIF"));
    CHECK(RotateLossless::probe(QStringLiteral("x.hdr")).reason == QStringLiteral("HDR"));
    CHECK(RotateLossless::probe(QStringLiteral("x.exr")).reason == QStringLiteral("EXR"));

    // 3. 临时目录生成 JPEG：无方向标签 → 补写标签 → 重读 → 再转 90° 原地改；
    //    两次补丁之间熵扫描区逐字节一致。
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString jpg = dir.path() + QStringLiteral("/sample.jpg");
    {
        QImage img(64, 48, QImage::Format_RGB32);
        img.fill(Qt::blue);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x)
                if ((x + y) % 7 == 0)
                    img.setPixel(x, y, qRgb(255, 200, 0));
        QImageWriter w(jpg, "JPG");
        w.setQuality(95);
        CHECK(w.write(img));
    }
    {
        const RotateLossless::Probe p = RotateLossless::probe(jpg);
        CHECK(p.supported);
        CHECK(RotateLossless::readOrientation(jpg) == 0);
    }
    QByteArray beforeScan;
    {
        QFile f(jpg);
        f.open(QIODevice::ReadOnly);
        beforeScan = jpegScan(f.readAll());
        CHECK(!beforeScan.isEmpty());
    }
    QString err;
    CHECK(RotateLossless::writeOrientation(jpg, 6, err));
    CHECK(RotateLossless::readOrientation(jpg) == 6);
    {
        QFile f(jpg);
        f.open(QIODevice::ReadOnly);
        CHECK(jpegScan(f.readAll()) == beforeScan);   // 压缩扫描区逐字节不变
    }
    CHECK(RotateLossless::writeOrientation(jpg, 3, err));
    CHECK(RotateLossless::readOrientation(jpg) == 3);
    {
        QFile f(jpg);
        f.open(QIODevice::ReadOnly);
        CHECK(jpegScan(f.readAll()) == beforeScan);
    }

    // 4. WebP 往返回归：写回后必须能被自身解析器重开（chunk 长度 / RIFF 尺寸合法），
    //    方向标签可重读；覆盖早期 chunk 构造未扩容、长度字段被 TIFF 头覆盖的缺陷。
    const QString webp = dir.path() + QStringLiteral("/sample.webp");
    {
        QImage img(64, 48, QImage::Format_RGB32);
        img.fill(Qt::darkGreen);
        QImageWriter w(webp, "WEBP");
        CHECK(w.write(img));
    }
    {
        const RotateLossless::Probe p0 = RotateLossless::probe(webp);
        CHECK(p0.supported);
        CHECK(RotateLossless::writeOrientation(webp, 6, err));
        const RotateLossless::Probe p1 = RotateLossless::probe(webp);
        CHECK(p1.supported);                              // 写回产物可重新解析
        CHECK(RotateLossless::readOrientation(webp) == 6);
        CHECK(RotateLossless::writeOrientation(webp, 3, err));
        const RotateLossless::Probe p2 = RotateLossless::probe(webp);
        CHECK(p2.supported);
        CHECK(RotateLossless::readOrientation(webp) == 3);
    }

    // 5. 同一张 JPEG 复制成 PNG 后缀：probe 跳过、写回失败，整文件 SHA256 不变。
    const QString pngCopy = dir.path() + QStringLiteral("/copy.png");
    {
        QFile::copy(jpg, pngCopy);
        QFile f(pngCopy);
        f.open(QIODevice::ReadOnly);
        const QByteArray orig = f.readAll();
        const QString origHash = sha256(orig);
        CHECK(!RotateLossless::probe(pngCopy).supported);
        CHECK(!RotateLossless::writeOrientation(pngCopy, 6, err));
        f.seek(0);
        CHECK(sha256(f.readAll()) == origHash);
    }

    // 6. 只读文件：干净失败，原文件完好。
    {
        const QString ro = dir.path() + QStringLiteral("/readonly.jpg");
        QFile::copy(jpg, ro);
        QFile::setPermissions(ro, QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther);
        QFile f(ro);
        f.open(QIODevice::ReadOnly);
        const QByteArray orig = f.readAll();
        const QString origHash = sha256(orig);
        CHECK(!RotateLossless::writeOrientation(ro, 8, err));
        QFile::setPermissions(ro, QFile::WriteUser | QFile::ReadUser);
        f.seek(0);
        CHECK(sha256(f.readAll()) == origHash);
    }

    // 7. 不存在的文件：干净失败。
    CHECK(!RotateLossless::writeOrientation(dir.path() + QStringLiteral("/nope.jpg"), 6, err));

    return g_failures == 0 ? 0 : 1;
}
