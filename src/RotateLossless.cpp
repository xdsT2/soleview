#include "RotateLossless.h"

#include "ArchiveStore.h"
#include "ExtraImageLoader.h"
#include "RawImageLoader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRandomGenerator>
#include <QSet>
#include <QThread>

#include <io.h>
#include <windows.h>

#include <array>

namespace RotateLossless {

namespace {

// —— 原因码（与回执矩阵 / 去重集合对齐；用户可见文案由宿主统一翻译）——
constexpr const char *kReasonPng = "PNG";
constexpr const char *kReasonBmp = "BMP";
constexpr const char *kReasonGif = "GIF";
constexpr const char *kReasonHdr = "HDR";
constexpr const char *kReasonExr = "EXR";
constexpr const char *kReasonAvif = "AVIF";
constexpr const char *kReasonRaw = "RAW";
constexpr const char *kReasonPsd = "PSD";
constexpr const char *kReasonAnim = "ANIM";
constexpr const char *kReasonAnimWebp = "ANIM_WEBP";
constexpr const char *kReasonMultiTiff = "MULTITIFF";
constexpr const char *kReasonBigTiff = "BIGTIFF";
constexpr const char *kReasonHeifNoIrot = "HEIF_NO_IROT";
constexpr const char *kReasonHeifMirror = "HEIF_MIRROR";
constexpr const char *kReasonHeifExifItem = "HEIF_EXIF_ITEM";
constexpr const char *kReasonVp8lNoVp8x = "VP8L_NO_VP8X";
constexpr const char *kReasonArchiveEntry = "ARCHIVE_ENTRY";
constexpr const char *kReasonStructure = "STRUCTURE";
constexpr const char *kReasonOther = "OTHER";

constexpr quint16 kTagOrientation = 0x0112;
constexpr quint16 kTypeShort = 3;
constexpr quint32 kCountOne = 1;

// —— 小端 / 大端读写 ——
quint16 rd16(const QByteArray &b, int off, bool le)
{
    const quint8 a = quint8(b[off]), c = quint8(b[off + 1]);
    return le ? quint16(a | (c << 8)) : quint16((a << 8) | c);
}

quint32 rd32(const QByteArray &b, int off, bool le)
{
    const quint32 a = quint8(b[off]), c = quint8(b[off + 1]),
                  d = quint8(b[off + 2]), e = quint8(b[off + 3]);
    return le ? (a | (c << 8) | (d << 16) | (e << 24))
              : ((a << 24) | (c << 16) | (d << 8) | e);
}

void wr16(QByteArray &b, int off, quint16 v, bool le)
{
    b[off] = char(le ? (v & 0xff) : (v >> 8));
    b[off + 1] = char(le ? (v >> 8) : (v & 0xff));
}

void wr32(QByteArray &b, int off, quint32 v, bool le)
{
    for (int i = 0; i < 4; ++i) {
        const int shift = le ? i * 8 : (3 - i) * 8;
        b[off + i] = char((v >> shift) & 0xff);
    }
}

// —— TIFF / EXIF-TIFF 块（只读解析 + 方向标签改写，口径同阶段一 tiff_edit.py）——
class TiffBlock {
public:
    bool le = true;
    quint32 ifd0 = 0;
    bool valid = false;

    static TiffBlock parse(const QByteArray &d, int base, QString &err)
    {
        TiffBlock tb;
        tb.valid = false;
        if (base + 8 > d.size()) {
            err = QStringLiteral("TIFF 头越界");
            return tb;
        }
        const QByteArray order = d.mid(base, 2);
        if (order == "II") {
            tb.le = true;
        } else if (order == "MM") {
            tb.le = false;
        } else {
            err = QStringLiteral("非法字节序");
            return tb;
        }
        if (rd16(d, base + 2, tb.le) != 42) {
            err = QStringLiteral("非法 TIFF 魔数");
            return tb;
        }
        tb.ifd0 = rd32(d, base + 4, tb.le);
        if (tb.ifd0 < 8 || quint64(tb.ifd0) + 2 > quint64(d.size() - base)) {
            err = QStringLiteral("IFD0 偏移越界");
            return tb;
        }
        tb.valid = true;
        return tb;
    }

    struct Entry {
        quint16 tag = 0, type = 0;
        quint32 count = 0, value = 0;
        int abs = 0;
    };

    QList<Entry> entries(const QByteArray &d, int base, int ifdRel) const
    {
        QList<Entry> out;
        const int off = base + ifdRel;
        const quint16 n = rd16(d, off, le);
        for (quint16 i = 0; i < n; ++i) {
            const int e = off + 2 + i * 12;
            if (e + 12 > d.size())
                break;
            Entry ent;
            ent.tag = rd16(d, e, le);
            ent.type = rd16(d, e + 2, le);
            ent.count = rd32(d, e + 4, le);
            ent.value = rd32(d, e + 8, le);
            ent.abs = e;
            out.append(ent);
        }
        return out;
    }

    quint32 nextIfd(const QByteArray &d, int base, int ifdRel) const
    {
        const int off = base + ifdRel;
        const quint16 n = rd16(d, off, le);
        const int tail = off + 2 + n * 12;
        if (tail + 4 > d.size())
            return 0;
        return rd32(d, tail, le);
    }

    int orientation(const QByteArray &d, int base) const
    {
        for (const Entry &e : entries(d, base, ifd0)) {
            if (e.tag == kTagOrientation) {
                if (e.type != kTypeShort || e.count != kCountOne)
                    return -1;   // 非内联 SHORT：拒绝改写
                return rd16(d, e.abs + 8, le);
            }
        }
        return 0;
    }

    // 返回新的 TIFF 块字节。已有标签原地改 2 字节；缺失则块尾重建 IFD0 追加条目。
    QByteArray setOrientation(const QByteArray &d, int base, int value) const
    {
        if (value < 1 || value > 8)
            return {};
        const QList<Entry> old = entries(d, base, ifd0);
        for (const Entry &e : old) {
            if (e.tag == kTagOrientation) {
                if (e.type != kTypeShort || e.count != kCountOne)
                    return {};
                QByteArray out = d;
                wr16(out, e.abs + 8, quint16(value), le);
                return out;
            }
        }
        // 无标签：块尾追加新 IFD0（旧条目原样抄入；值区偏移相对块起点不变，依然有效）。
        const quint32 next = nextIfd(d, base, ifd0);
        const int newIfdRel = d.size() - base;
        const int oldCount = old.size();
        QByteArray add;
        add.resize(2 + oldCount * 12 + 12 + 4);
        wr16(add, 0, quint16(oldCount + 1), le);
        for (int i = 0; i < oldCount; ++i) {
            add.replace(2 + i * 12, 12, d.mid(old[i].abs, 12));
        }
        const int e = 2 + oldCount * 12;
        wr16(add, e, kTagOrientation, le);
        wr16(add, e + 2, kTypeShort, le);
        wr32(add, e + 4, kCountOne, le);
        wr16(add, e + 8, quint16(value), le);
        wr16(add, e + 10, 0, le);
        wr32(add, e + 12, next, le);
        QByteArray out = d + add;
        wr32(out, base + 4, quint32(newIfdRel), le);
        return out;
    }
};

// —— JPEG 段走查 ——
struct JpegSeg {
    quint8 marker = 0;
    int start = 0;
    int end = 0;
    int payloadStart = 0;
    int payloadEnd = 0;
};

bool parseJpeg(const QByteArray &data, QList<JpegSeg> &segs, int &exifIdx, QString &err)
{
    exifIdx = -1;
    if (data.size() < 4 || quint8(data[0]) != 0xFF || quint8(data[1]) != 0xD8) {
        err = QStringLiteral("不是 JPEG（无 SOI）");
        return false;
    }
    int pos = 2;
    while (pos + 4 <= data.size()) {
        if (quint8(data[pos]) != 0xFF) {
            err = QStringLiteral("段走查失败");
            return false;
        }
        const quint8 m = quint8(data[pos + 1]);
        if (m == 0xD8) {
            pos += 2;
            continue;
        }
        if (m == 0xDA) {   // SOS：其后为熵扫描，不再有元数据段
            const int ln = rd16(data, pos + 2, false);
            JpegSeg seg;
            seg.marker = m;
            seg.start = pos;
            seg.end = data.size();
            seg.payloadStart = pos + 2 + ln;
            seg.payloadEnd = data.size();
            segs.append(seg);
            return true;
        }
        if (m == 0xD9)
            return true;
        const int ln = rd16(data, pos + 2, false);
        if (ln < 2 || pos + 2 + ln > data.size()) {
            err = QStringLiteral("段长非法");
            return false;
        }
        JpegSeg seg;
        seg.marker = m;
        seg.start = pos;
        seg.end = pos + 2 + ln;
        seg.payloadStart = pos + 4;
        seg.payloadEnd = pos + 2 + ln;
        if (m == 0xE1 && ln >= 8 && data.mid(pos + 4, 6) == QByteArray("Exif\0\0", 6))
            exifIdx = segs.size();
        segs.append(seg);
        pos += 2 + ln;
    }
    err = QStringLiteral("未找到 SOS 段");
    return false;
}

QByteArray buildMinimalTiff(int orientation)
{
    // 8 字节头；IFD0 @8：计数 u16、12 字节条目、next IFD u32，共 26 字节。
    QByteArray tiff;
    tiff.resize(26);
    tiff[0] = 'I';
    tiff[1] = 'I';
    wr16(tiff, 2, 42, true);
    wr32(tiff, 4, 8, true);
    wr16(tiff, 8, 1, true);
    const int entry = 10;
    wr16(tiff, entry, kTagOrientation, true);
    wr16(tiff, entry + 2, kTypeShort, true);
    wr32(tiff, entry + 4, kCountOne, true);
    wr16(tiff, entry + 8, quint16(orientation), true);
    wr16(tiff, entry + 10, 0, true);
    wr32(tiff, 22, 0, true);
    return tiff;
}

QByteArray patchJpeg(const QByteArray &data, int newOrient, QString &err)
{
    QList<JpegSeg> segs;
    int exifIdx = -1;
    if (!parseJpeg(data, segs, exifIdx, err))
        return {};

    if (exifIdx >= 0) {
        const JpegSeg &seg = segs[exifIdx];
        const int tiffBase = seg.payloadStart + 6;
        const QByteArray tiffSlice = data.mid(tiffBase, seg.payloadEnd - tiffBase);
        QString terr;
        TiffBlock tb = TiffBlock::parse(tiffSlice, 0, terr);
        if (!tb.valid) {
            err = QStringLiteral("EXIF 解析失败：%1").arg(terr);
            return {};
        }
        const QByteArray newTiff = tb.setOrientation(tiffSlice, 0, newOrient);
        if (newTiff.isEmpty()) {
            err = QStringLiteral("方向标签改写失败");
            return {};
        }
        QByteArray payload("Exif\0\0", 6);
        payload += newTiff;
        if (payload.size() + 2 > 0xFFFF) {
            err = QStringLiteral("APP1 段长超 64KB");
            return {};
        }
        QByteArray newSeg;
        newSeg.resize(2 + 2 + payload.size());
        newSeg[0] = char(0xFF);
        newSeg[1] = char(0xE1);
        wr16(newSeg, 2, quint16(payload.size() + 2), false);
        newSeg.replace(4, payload.size(), payload);
        QByteArray out = data.left(seg.start) + newSeg + data.mid(seg.end);
        QString vErr;
        int vIdx;
        parseJpeg(out, segs, vIdx, vErr);   // 复检可重新解析
        return out;
    }

    // 无 Exif：插入 APP1。位置 SOI 之后；若 APP0(JFIF) 紧随其后则排在它后面。
    const QByteArray tiff = buildMinimalTiff(newOrient);
    QByteArray payload("Exif\0\0", 6);
    payload += tiff;
    QByteArray app1;
    app1.resize(2 + 2 + payload.size());
    app1[0] = char(0xFF);
    app1[1] = char(0xE1);
    wr16(app1, 2, quint16(payload.size() + 2), false);
    app1.replace(4, payload.size(), payload);

    int insertAt = 2;
    for (const JpegSeg &s : segs) {
        if (s.marker == 0xE0 && s.start == insertAt) {
            insertAt = s.end;
            break;
        }
    }
    return data.left(insertAt) + app1 + data.mid(insertAt);
}

// —— WebP (RIFF) chunk 走查 ——
struct RiffChunk {
    QByteArray fourcc;
    int start = 0;
    int payloadStart = 0;
    int payloadEnd = 0;
    int end = 0;
    quint32 chunkSize = 0;
};

bool parseRiff(const QByteArray &data, QList<RiffChunk> &chunks, QString &err)
{
    if (data.size() < 12 || data.mid(0, 4) != "RIFF" || data.mid(8, 4) != "WEBP") {
        err = QStringLiteral("不是 WebP RIFF");
        return false;
    }
    int p = 12;
    while (p + 8 <= data.size()) {
        RiffChunk c;
        c.fourcc = data.mid(p, 4);
        c.chunkSize = rd32(data, p + 4, true);
        if (p + 8 + int(c.chunkSize) > data.size()) {
            err = QStringLiteral("chunk 越界");
            return false;
        }
        c.start = p;
        c.payloadStart = p + 8;
        c.payloadEnd = p + 8 + int(c.chunkSize);
        c.end = p + 8 + int(c.chunkSize) + (c.chunkSize & 1);
        chunks.append(c);
        p = c.end;
    }
    return true;
}

const RiffChunk *findChunk(const QList<RiffChunk> &chunks, const char *fourcc)
{
    for (const RiffChunk &c : chunks) {
        if (c.fourcc == QByteArray(fourcc))
            return &c;
    }
    return nullptr;
}

QByteArray buildVp8x(quint8 flags, int w, int h)
{
    QByteArray body;
    body.resize(10);
    body[0] = char(flags);
    body[1] = char(0);
    body[2] = char(0);
    body[3] = char(0);
    const quint32 wm1 = quint32(w - 1), hm1 = quint32(h - 1);
    body[4] = char(wm1 & 0xff);
    body[5] = char((wm1 >> 8) & 0xff);
    body[6] = char((wm1 >> 16) & 0xff);
    body[7] = char(hm1 & 0xff);
    body[8] = char((hm1 >> 8) & 0xff);
    body[9] = char((hm1 >> 16) & 0xff);
    QByteArray chunk;
    chunk.resize(8);
    chunk.replace(0, 4, "VP8X");
    wr32(chunk, 4, quint32(body.size()), true);
    chunk += body;
    return chunk;
}

QByteArray patchWebp(const QByteArray &data, int newOrient, QString &err)
{
    QList<RiffChunk> chunks;
    if (!parseRiff(data, chunks, err))
        return {};

    const RiffChunk *exif = findChunk(chunks, "EXIF");
    const RiffChunk *vp8x = findChunk(chunks, "VP8X");

    if (exif) {
        QByteArray payload = data.mid(exif->payloadStart, exif->payloadEnd - exif->payloadStart);
        bool stripped = payload.size() >= 6 && payload.mid(0, 6) == QByteArray("Exif\0\0", 6);
        const QByteArray tiffBytes = stripped ? payload.mid(6) : payload;
        QString terr;
        TiffBlock tb = TiffBlock::parse(tiffBytes, 0, terr);
        if (!tb.valid) {
            err = QStringLiteral("WebP EXIF TIFF 解析失败：%1").arg(terr);
            return {};
        }
        const QByteArray newTiff = tb.setOrientation(tiffBytes, 0, newOrient);
        if (newTiff.isEmpty()) {
            err = QStringLiteral("WebP 方向标签改写失败");
            return {};
        }
        QByteArray newPayload;
        if (stripped)
            newPayload += QByteArray("Exif\0\0", 6);
        newPayload += newTiff;
        QByteArray newChunk;
        newChunk.resize(8);
        newChunk.replace(0, 4, "EXIF");
        wr32(newChunk, 4, quint32(newPayload.size()), true);
        newChunk += newPayload;
        if (newPayload.size() & 1)
            newChunk += char(0);
        QByteArray out = data.left(exif->start) + newChunk + data.mid(exif->end);
        wr32(out, 4, quint32(out.size() - 8), true);
        return out;
    }

    // 新增 EXIF chunk。
    const QByteArray tiff = buildMinimalTiff(newOrient);
    QByteArray newChunk;
    newChunk.resize(8);
    newChunk.replace(0, 4, "EXIF");
    wr32(newChunk, 4, quint32(tiff.size()), true);
    newChunk += tiff;
    if (tiff.size() & 1)
        newChunk += char(0);

    QByteArray out = data;
    if (vp8x) {
        // 置 EXIF 标志位（VP8X 载荷首字节 flags，位 0x08）。
        out[vp8x->payloadStart] = char(quint8(out[vp8x->payloadStart]) | 0x08);
        int insert = vp8x->end;
        if (const RiffChunk *iccp = findChunk(chunks, "ICCP"))
            insert = iccp->end;
        out = out.left(insert) + newChunk + out.mid(insert);
    } else {
        const RiffChunk *vp8 = findChunk(chunks, "VP8 ");
        const RiffChunk *vp8l = findChunk(chunks, "VP8L");
        int w = 0, h = 0;
        if (vp8) {
            const int ps = vp8->payloadStart;
            if (ps + 10 > data.size()) {
                err = QStringLiteral("VP8 帧头越界");
                return {};
            }
            w = rd16(data, ps + 6, true) & 0x3FFF;
            h = rd16(data, ps + 8, true) & 0x3FFF;
        } else if (vp8l) {
            err = QStringLiteral("VP8L 无 VP8X：合成 VP8X 需位流解析尺寸，跳过");
            return {};
        }
        if (w == 0 || h == 0) {
            err = QStringLiteral("无法解析画布尺寸，拒绝合成 VP8X");
            return {};
        }
        out = data.left(12) + buildVp8x(0x08, w, h) + newChunk + data.mid(12);
    }
    wr32(out, 4, quint32(out.size() - 8), true);
    QList<RiffChunk> vChunks;
    QString vErr;
    parseRiff(out, vChunks, vErr);   // 复检
    return out;
}

// —— 裸 TIFF ——
bool tiffWalkPages(const QByteArray &data, const TiffBlock &tb, int &pages)
{
    QSet<quint32> seen;
    quint32 off = tb.ifd0;
    pages = 0;
    while (off && !seen.contains(off)) {
        seen.insert(off);
        ++pages;
        if (off + 2 > quint32(data.size()))
            return false;
        const quint16 n = rd16(data, off, tb.le);
        const int tail = off + 2 + n * 12;
        if (tail + 4 > data.size())
            return false;
        off = rd32(data, tail, tb.le);
    }
    return true;
}

bool detectBigTiff(const QByteArray &data)
{
    if (data.size() < 4)
        return false;
    if (data.mid(0, 2) != "II" && data.mid(0, 2) != "MM")
        return false;
    return rd16(data, 2, data.mid(0, 2) == "II") == 43;
}

QByteArray patchTiff(const QByteArray &data, int newOrient, QString &err)
{
    if (detectBigTiff(data)) {
        err = QStringLiteral("BigTIFF：条目 / 偏移宽度不同，跳过");
        return {};
    }
    QString terr;
    TiffBlock tb = TiffBlock::parse(data, 0, terr);
    if (!tb.valid) {
        err = QStringLiteral("TIFF 解析失败：%1").arg(terr);
        return {};
    }
    int pages = 0;
    if (!tiffWalkPages(data, tb, pages) || pages > 1) {
        err = QStringLiteral("多页 TIFF：方向归属不唯一，跳过");
        return {};
    }
    const QByteArray out = tb.setOrientation(data, 0, newOrient);
    if (out.isEmpty()) {
        err = QStringLiteral("方向标签改写失败");
        return {};
    }
    return out;
}

// —— HEIC / HEIF (ISOBMFF) ——
struct Box {
    QByteArray type;
    int start = 0;
    int hdr = 0;
    int size = 0;
    int body = 0;
};

QList<Box> readBoxes(const QByteArray &data, int start, int end)
{
    QList<Box> out;
    int p = start;
    while (p + 8 <= end) {
        quint32 size = rd32(data, p, false);
        QByteArray type = data.mid(p + 4, 4);
        int hdr = 8;
        if (size == 1) {
            if (p + 16 > end)
                break;
            // 64 位扩展大小：高 32 位本阶段不支持，按不可解析处理。
            size = rd32(data, p + 12, false);
            hdr = 16;
        } else if (size == 0) {
            size = quint32(end - p);
        }
        if (size < quint32(hdr) || p + int(size) > end)
            break;
        Box b;
        b.type = type;
        b.start = p;
        b.hdr = hdr;
        b.size = int(size);
        b.body = p + hdr;
        out.append(b);
        p += int(size);
    }
    return out;
}

struct HeifInfo {
    bool parsed = false;
    int primary = -1;
    bool hasIrot = false;
    int irotCode = 0;
    bool hasImir = false;
    bool hasExifItem = false;
};

bool parseHeif(const QByteArray &data, HeifInfo &info, QString &err)
{
    info = HeifInfo();
    if (data.size() < 8 || data.mid(4, 4) != "ftyp") {
        err = QStringLiteral("非 ISOBMFF / HEIF");
        return false;
    }
    const QList<Box> top = readBoxes(data, 0, data.size());
    Box meta;
    bool foundMeta = false;
    for (const Box &b : top) {
        if (b.type == "meta") {
            meta = b;
            foundMeta = true;
            break;
        }
    }
    if (!foundMeta) {
        err = QStringLiteral("缺 meta box");
        return false;
    }
    // meta 头：version(1) + flags(3)，子盒从 body+4 起。
    const QList<Box> children = readBoxes(data, meta.body + 4, meta.start + meta.size);

    Box pitm, iloc, iinf, iprp;
    for (const Box &b : children) {
        if (b.type == "pitm")
            pitm = b;
        else if (b.type == "iloc")
            iloc = b;
        else if (b.type == "iinf")
            iinf = b;
        else if (b.type == "iprp")
            iprp = b;
    }
    if (pitm.size) {
        const quint8 ver = quint8(data[pitm.body]);
        info.primary = ver == 0 ? int(rd16(data, pitm.body + 4, false))
                                 : int(rd32(data, pitm.body + 4, false));
    }

    // iinf：列出 item 类型（找 Exif item）。
    if (iinf.size) {
        const QList<Box> infe = readBoxes(data, iinf.body, iinf.start + iinf.size);
        for (const Box &ib : infe) {
            if (ib.type != "infe")
                continue;
            const quint8 ver = quint8(data[ib.body]);
            int p = ib.body + 4;
            if (ver >= 2) {
                p += 4;   // item_id
                p += 2;   // item_protection_index
                if (p + 4 <= ib.start + ib.size) {
                    if (data.mid(p, 4) == "Exif")
                        info.hasExifItem = true;
                }
            }
        }
    }

    if (!iprp.size) {
        info.parsed = true;
        return true;
    }
    const QList<Box> iprpChildren = readBoxes(data, iprp.body, iprp.start + iprp.size);
    Box ipco, ipma;
    for (const Box &b : iprpChildren) {
        if (b.type == "ipco")
            ipco = b;
        else if (b.type == "ipma")
            ipma = b;
    }
    if (!ipco.size || !ipma.size) {
        info.parsed = true;
        return true;
    }
    const QList<Box> props = readBoxes(data, ipco.body, ipco.start + ipco.size);

    // ipma 绑定表。
    const quint8 ver = quint8(data[ipma.body]);
    const quint32 flags = (quint8(data[ipma.body + 1]) << 16)
                          | (quint8(data[ipma.body + 2]) << 8)
                          | quint8(data[ipma.body + 3]);
    int p = ipma.body + 4;
    const quint32 assoc = rd32(data, p, false) & 0x7FFFFFFF;
    p += 4;
    const bool idx16 = (flags & 1) != 0;
    QHash<int, QList<int>> mapping;
    for (quint32 i = 0; i < assoc; ++i) {
        int iid = 0;
        if (ver < 1) {
            iid = int(rd16(data, p, false));
            p += 2;
        } else {
            iid = int(rd32(data, p, false));
            p += 4;
        }
        const quint8 n = quint8(data[p]);
        ++p;
        QList<int> plist;
        for (quint8 k = 0; k < n; ++k) {
            int idx = 0;
            if (idx16) {
                idx = int(rd16(data, p, false) & 0x7FFF);
                p += 2;
            } else {
                idx = int(quint8(data[p]) & 0x7F);
                p += 1;
            }
            plist.append(idx);
        }
        mapping[iid] = plist;
    }

    const QList<int> plist = mapping.value(info.primary);
    for (int idx : plist) {
        if (idx < 1 || idx > props.size())
            continue;
        const Box &pb = props[idx - 1];
        if (pb.type == "irot") {
            info.hasIrot = true;
            info.irotCode = quint8(data[pb.body]) & 0x3F;
        } else if (pb.type == "imir") {
            info.hasImir = true;
        }
    }
    info.parsed = true;
    return true;
}

// EXIF 纯旋转子集 → irot 编码：1→0, 3→2, 6→3, 8→1。
int tagToIrotCode(int tag)
{
    switch (tag) {
    case 1: return 0;
    case 3: return 2;
    case 6: return 3;
    case 8: return 1;
    default: return -1;
    }
}

QByteArray patchHeif(const QByteArray &data, int newOrient, QString &err)
{
    HeifInfo info;
    if (!parseHeif(data, info, err))
        return {};
    const int code = tagToIrotCode(newOrient);
    if (code < 0) {
        err = QStringLiteral("HEIC 镜像方向不在原位改写范围，跳过");
        return {};
    }
    if (!info.hasIrot) {
        err = QStringLiteral("主 item 无 irot（新增 irot 属结构重写，跳过）");
        return {};
    }
    if (info.hasImir) {
        err = QStringLiteral("HEIC 含镜像属性，跳过");
        return {};
    }

    // 重新定位主 item 绑定的 irot 盒并原位改角度字节（盒定长，长度不变）。
    const QList<Box> top = readBoxes(data, 0, data.size());
    Box meta;
    for (const Box &b : top) {
        if (b.type == "meta") {
            meta = b;
            break;
        }
    }
    const QList<Box> children = readBoxes(data, meta.body + 4, meta.start + meta.size);
    Box iprp;
    for (const Box &b : children) {
        if (b.type == "iprp")
            iprp = b;
    }
    const QList<Box> iprpChildren = readBoxes(data, iprp.body, iprp.start + iprp.size);
    Box ipco, ipma;
    for (const Box &b : iprpChildren) {
        if (b.type == "ipco")
            ipco = b;
        else if (b.type == "ipma")
            ipma = b;
    }
    const QList<Box> props = readBoxes(data, ipco.body, ipco.start + ipco.size);
    const quint8 ver = quint8(data[ipma.body]);
    const quint32 flags = (quint8(data[ipma.body + 1]) << 16)
                           | (quint8(data[ipma.body + 2]) << 8)
                           | quint8(data[ipma.body + 3]);
    int p = ipma.body + 4;
    const quint32 assoc = rd32(data, p, false) & 0x7FFFFFFF;
    p += 4;
    const bool idx16 = (flags & 1) != 0;
    QHash<int, QList<int>> mapping;
    for (quint32 i = 0; i < assoc; ++i) {
        int iid = 0;
        if (ver < 1) {
            iid = int(rd16(data, p, false));
            p += 2;
        } else {
            iid = int(rd32(data, p, false));
            p += 4;
        }
        const quint8 n = quint8(data[p]);
        ++p;
        QList<int> plist;
        for (quint8 k = 0; k < n; ++k) {
            int idx = 0;
            if (idx16) {
                idx = int(rd16(data, p, false) & 0x7FFF);
                p += 2;
            } else {
                idx = int(quint8(data[p]) & 0x7F);
                p += 1;
            }
            plist.append(idx);
        }
        mapping[iid] = plist;
    }

    QByteArray out = data;
    bool patched = false;
    for (int idx : mapping.value(info.primary)) {
        if (idx < 1 || idx > props.size())
            continue;
        const Box &pb = props[idx - 1];
        if (pb.type == "irot") {
            out[pb.body] = char(code);
            patched = true;
        }
    }
    if (!patched) {
        err = QStringLiteral("irot 绑定定位失败");
        return {};
    }
    return out;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

}   // namespace

// —— D4 合成表（实证 ground truth；应用内右转 = 屏幕顺时针 90°）——
int composeRotateCw(int orig)
{
    static const std::array<int, 9> t = { 0, 6, 7, 8, 5, 2, 3, 4, 1 };
    const int c = (orig >= 1 && orig <= 8) ? orig : 1;
    return t[c];
}

int composeRotateCcw(int orig)
{
    static const std::array<int, 9> t = { 0, 8, 5, 6, 7, 4, 1, 2, 3 };
    const int c = (orig >= 1 && orig <= 8) ? orig : 1;
    return t[c];
}

int composeRotate180(int orig)
{
    static const std::array<int, 9> t = { 0, 3, 4, 1, 2, 7, 8, 5, 6 };
    const int c = (orig >= 1 && orig <= 8) ? orig : 1;
    return t[c];
}

int composeFlipH(int orig)
{
    static const std::array<int, 9> t = { 0, 2, 1, 4, 3, 6, 5, 8, 7 };
    const int c = (orig >= 1 && orig <= 8) ? orig : 1;
    return t[c];
}

int composeFlipV(int orig)
{
    static const std::array<int, 9> t = { 0, 4, 3, 2, 1, 8, 7, 6, 5 };
    const int c = (orig >= 1 && orig <= 8) ? orig : 1;
    return t[c];
}

Probe probe(const QString &path)
{
    Probe out;
    if (ArchiveStore::isEntryPath(path)) {
        out.reason = QLatin1String(kReasonArchiveEntry);
        return out;
    }

    const QString suf = QFileInfo(path).suffix().toLower();
    // 后缀即可判定的不支持格式：不要求文件存在（验收探针 / 纯结构判定场景）。
    if (suf == QLatin1String("png")) {
        out.reason = QLatin1String(kReasonPng);
        return out;
    }
    if (suf == QLatin1String("bmp") || suf == QLatin1String("dib")) {
        out.reason = QLatin1String(kReasonBmp);
        return out;
    }
    if (suf == QLatin1String("gif")) {
        out.reason = QLatin1String(kReasonGif);
        return out;
    }
    if (suf == QLatin1String("hdr") || suf == QLatin1String("hdri") || suf == QLatin1String("rgbe")) {
        out.reason = QLatin1String(kReasonHdr);
        return out;
    }
    if (suf == QLatin1String("exr")) {
        out.reason = QLatin1String(kReasonExr);
        return out;
    }
    if (suf == QLatin1String("avif") || suf == QLatin1String("avifs")) {
        out.reason = QLatin1String(kReasonAvif);
        return out;
    }
    if (suf == QLatin1String("psd") || suf == QLatin1String("psb")) {
        out.reason = QLatin1String(kReasonPsd);
        return out;
    }
    if (isRawFilePath(path)) {
        out.reason = QLatin1String(kReasonRaw);
        return out;
    }

    QFileInfo fi(path);
    if (!fi.exists()) {
        out.reason = QStringLiteral("READ_FAILED");
        return out;
    }

    const QByteArray data = readFile(path);
    if (data.isEmpty()) {
        out.reason = QStringLiteral("READ_FAILED");
        return out;
    }

    if (suf == QLatin1String("jpg") || suf == QLatin1String("jpeg") || suf == QLatin1String("jpe")) {
        QList<JpegSeg> segs;
        int exifIdx = -1;
        QString err;
        if (parseJpeg(data, segs, exifIdx, err)) {
            out.supported = true;
        } else {
            out.reason = QLatin1String(kReasonStructure);
            out.detail = err;
        }
        return out;
    }

    if (suf == QLatin1String("webp")) {
        QList<RiffChunk> chunks;
        QString err;
        if (!parseRiff(data, chunks, err)) {
            out.reason = QLatin1String(kReasonStructure);
            out.detail = err;
            return out;
        }
        for (const RiffChunk &c : chunks) {
            if (c.fourcc == "ANIM" || c.fourcc == "ANMF") {
                out.reason = QLatin1String(kReasonAnimWebp);
                return out;
            }
        }
        const bool hasVp8x = findChunk(chunks, "VP8X") != nullptr;
        const bool hasVp8l = findChunk(chunks, "VP8L") != nullptr;
        if (hasVp8l && !hasVp8x) {
            out.reason = QLatin1String(kReasonVp8lNoVp8x);
            return out;
        }
        out.supported = true;
        return out;
    }

    if (suf == QLatin1String("heic") || suf == QLatin1String("heif")
        || suf == QLatin1String("heics") || suf == QLatin1String("avifs")) {
        HeifInfo info;
        QString err;
        if (!parseHeif(data, info, err)) {
            out.reason = QLatin1String(kReasonStructure);
            out.detail = err;
            return out;
        }
        if (info.hasImir) {
            out.reason = QLatin1String(kReasonHeifMirror);
            return out;
        }
        if (!info.hasIrot) {
            out.reason = info.hasExifItem ? QLatin1String(kReasonHeifExifItem)
                                           : QLatin1String(kReasonHeifNoIrot);
            return out;
        }
        out.supported = true;
        return out;
    }

    if (suf == QLatin1String("tif") || suf == QLatin1String("tiff")) {
        if (detectBigTiff(data)) {
            out.reason = QLatin1String(kReasonBigTiff);
            return out;
        }
        QString terr;
        TiffBlock tb = TiffBlock::parse(data, 0, terr);
        if (!tb.valid) {
            out.reason = QLatin1String(kReasonStructure);
            out.detail = terr;
            return out;
        }
        int pages = 0;
        if (!tiffWalkPages(data, tb, pages) || pages > 1) {
            out.reason = QLatin1String(kReasonMultiTiff);
            return out;
        }
        out.supported = true;
        return out;
    }

    if (ExtraImageLoader::hasMultipleFrames(path)) {
        out.reason = QLatin1String(kReasonAnim);
        return out;
    }
    out.reason = QLatin1String(kReasonOther);
    return out;
}

int readOrientation(const QString &path)
{
    const QByteArray data = readFile(path);
    if (data.isEmpty())
        return 0;
    const QString suf = QFileInfo(path).suffix().toLower();
    QString err;

    if (suf == QLatin1String("jpg") || suf == QLatin1String("jpeg") || suf == QLatin1String("jpe")) {
        QList<JpegSeg> segs;
        int exifIdx = -1;
        if (!parseJpeg(data, segs, exifIdx, err) || exifIdx < 0)
            return 0;
        const JpegSeg &seg = segs[exifIdx];
        const QByteArray tiffSlice = data.mid(seg.payloadStart + 6, seg.payloadEnd - seg.payloadStart - 6);
        QString terr;
        TiffBlock tb = TiffBlock::parse(tiffSlice, 0, terr);
        if (!tb.valid)
            return 0;
        return tb.orientation(tiffSlice, 0);
    }

    if (suf == QLatin1String("webp")) {
        QList<RiffChunk> chunks;
        if (!parseRiff(data, chunks, err))
            return 0;
        const RiffChunk *exif = findChunk(chunks, "EXIF");
        if (!exif)
            return 0;
        QByteArray payload = data.mid(exif->payloadStart, exif->payloadEnd - exif->payloadStart);
        const bool stripped = payload.size() >= 6 && payload.mid(0, 6) == QByteArray("Exif\0\0", 6);
        const QByteArray tiffBytes = stripped ? payload.mid(6) : payload;
        QString terr;
        TiffBlock tb = TiffBlock::parse(tiffBytes, 0, terr);
        if (!tb.valid)
            return 0;
        return tb.orientation(tiffBytes, 0);
    }

    if (suf == QLatin1String("tif") || suf == QLatin1String("tiff")) {
        QString terr;
        TiffBlock tb = TiffBlock::parse(data, 0, terr);
        if (!tb.valid)
            return 0;
        return tb.orientation(data, 0);
    }

    if (suf == QLatin1String("heic") || suf == QLatin1String("heif")) {
        HeifInfo info;
        if (!parseHeif(data, info, err) || !info.hasIrot)
            return 0;
        // irot 编码 → EXIF 方向：0→1, 2→3, 3→6, 1→8。
        switch (info.irotCode) {
        case 0: return 1;
        case 2: return 3;
        case 3: return 6;
        case 1: return 8;
        default: return 0;
        }
    }
    return 0;
}

bool writeOrientation(const QString &path, int newOrientation, QString &err)
{
    if (newOrientation < 1 || newOrientation > 8) {
        err = QStringLiteral("方向值非法");
        return false;
    }
    const Probe p = probe(path);
    if (!p.supported) {
        err = p.reason;
        return false;
    }
    const QByteArray data = readFile(path);
    if (data.isEmpty()) {
        err = QStringLiteral("读取原文件失败");
        return false;
    }

    const QString suf = QFileInfo(path).suffix().toLower();
    QByteArray out;
    if (suf == QLatin1String("jpg") || suf == QLatin1String("jpeg") || suf == QLatin1String("jpe")) {
        out = patchJpeg(data, newOrientation, err);
    } else if (suf == QLatin1String("webp")) {
        out = patchWebp(data, newOrientation, err);
    } else if (suf == QLatin1String("tif") || suf == QLatin1String("tiff")) {
        out = patchTiff(data, newOrientation, err);
    } else if (suf == QLatin1String("heic") || suf == QLatin1String("heif")) {
        out = patchHeif(data, newOrientation, err);
    } else {
        err = QStringLiteral("不支持的格式");
        return false;
    }
    if (out.isEmpty())
        return false;

    // —— 原子写回：同目录临时文件 → flush/fsync → 原子替换；失败清理临时文件 ——
    QFileInfo fi(path);
    if (!fi.exists()) {
        err = QStringLiteral("文件不存在");
        return false;
    }
    if (!fi.isWritable()) {
        err = QStringLiteral("只读或无写权限");
        return false;
    }
    const QString dir = fi.dir().absolutePath();
    const QString tmp = dir + QLatin1Char('/') + fi.fileName()
                        + QStringLiteral(".lossless-%1-%2.tmp")
                              .arg(QCoreApplication::applicationPid())
                              .arg(QRandomGenerator::global()->generate());
    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            err = QStringLiteral("临时文件打开失败");
            return false;
        }
        if (f.write(out) != out.size()) {
            f.close();
            QFile::remove(tmp);
            err = QStringLiteral("临时文件写入失败");
            return false;
        }
        if (!f.flush() || _commit(f.handle()) != 0) {
            f.close();
            QFile::remove(tmp);
            err = QStringLiteral("临时文件落盘失败");
            return false;
        }
        f.close();
    }

    const std::wstring tmpW = tmp.toStdWString();
    const std::wstring dstW = path.toStdWString();
    bool replaced = false;
    for (int i = 0; i < 10 && !replaced; ++i) {
        if (i)
            QThread::msleep(150);
        replaced = MoveFileExW(tmpW.c_str(), dstW.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!replaced) {
        QFile::remove(tmp);
        err = QStringLiteral("替换原文件失败");
        return false;
    }
    return true;
}

}   // namespace RotateLossless
