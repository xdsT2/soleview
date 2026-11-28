#pragma once

#include <QSize>
#include <QtGlobal>

// 全量解码门限（BUG-20261127-02：亿级单色 TIF/BMP 闪退兜底）。
//
// 背景两条，缺一不可：
//   1) Qt 6 的 QImageReader 有一个**全局**「单次解码分配上限」（Qt 6.11 默认 256 MB，
//      见 QImageReader::allocationLimit）。超过它的图直接被拒，而且只说一句
//      「Unable to read image data」——SoleView 过去把它当成「文件可能已损坏」，
//      1.44 亿像素的单色 TIF 就是这么被误判的；门限内的 6400 万～1 亿像素的大图
//      也因此根本打不开。
//   2) 但只要把这个上限一味抬高而不设门限，32768×32768 的 1-bit TIFF 展开成 32 位
//      就是 4.3 GB 的 QImage，投哪台机器都是 OOM 闪退（用户报的就是这个）。
//
// 所以这里给一套显式口径，和 ExtraImageLoader / HdrExrDecode 一直用的门限同源，
// 也和发布说明里的「单边 3 万像素、总像素 1 亿」一致：
//   * 门限内：把分配上限抬到 kAllocationLimitMb，一律正常解码（不再有 256 MB 暗礁）；
//   * 门限外：解码前按 TooLarge 拒绝并说明尺寸，绝不去申请那块内存。
// 每个「会整图解码用户文件」的入口都必须在 read() 之前调用 exceeds()。
namespace DecodeLimits {

constexpr int kMaxDim = 30000;                 // 单边像素上限
constexpr quint64 kMaxPixels = 100000000ULL;   // 总像素上限（1 亿）
// 单次解码分配上限（MB）：1 亿像素按 32 位算 400 MB，按 16 位/通道（RGBA64）
// 算 800 MB，再留一点 Qt 内部缓冲余量，取 1024。
constexpr int kAllocationLimitMb = 1024;

// 尺寸是否超出全量解码门限。尺寸未知（无效 / 0）时返回 false：读不到头部就判死
// 太激进，交给解码器自己给结论。
inline bool exceeds(const QSize &size)
{
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) {
        return false;
    }
    if (size.width() > kMaxDim || size.height() > kMaxDim) {
        return true;
    }
    return quint64(size.width()) * quint64(size.height()) > kMaxPixels;
}

}   // namespace DecodeLimits
