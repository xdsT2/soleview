// DecodeLimits 纯逻辑单测（BUG-20261127-02：亿级单色 TIF/BMP 闪退门限）。
// 只验尺寸判定与门限常量：不碰解码器，也不需要样本文件。
#include "DecodeLimits.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            ++g_failures; \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

int main()
{
    // 1. 日常尺寸一律放行（含单边刚好等于上限、总像素刚好等于上限）
    CHECK(!DecodeLimits::exceeds(QSize(1920, 1080)));
    CHECK(!DecodeLimits::exceeds(QSize(8192, 8192)));
    CHECK(!DecodeLimits::exceeds(QSize(DecodeLimits::kMaxDim, 1000)));
    CHECK(!DecodeLimits::exceeds(QSize(10000, 10000)));   // 1 亿像素，正好压线

    // 2. 用户报的亿级样本：多页扫描/地图类单色 TIF 必须被拦下
    CHECK(DecodeLimits::exceeds(QSize(32768, 32768)));    // 10.7 亿像素
    CHECK(DecodeLimits::exceeds(QSize(12000, 12000)));    // 1.44 亿像素
    CHECK(DecodeLimits::exceeds(QSize(40000, 100)));      // 单边超限
    CHECK(DecodeLimits::exceeds(QSize(100, 30001)));      // 单边超限（另一轴）

    // 3. 单边压线但总像素超限 → 仍然拦下（两个门限任一命中即拦）
    CHECK(DecodeLimits::exceeds(QSize(DecodeLimits::kMaxDim, 20000)));

    // 4. 尺寸未知不判死：读不到头交给解码器给结论
    CHECK(!DecodeLimits::exceeds(QSize()));
    CHECK(!DecodeLimits::exceeds(QSize(0, 0)));
    CHECK(!DecodeLimits::exceeds(QSize(-1, -1)));

    // 5. 分配上限必须兜得住门限内的最坏情况：1 亿像素 × 16 位/通道（RGBA64）
    const quint64 worstBytes = DecodeLimits::kMaxPixels * 8ULL;
    CHECK(quint64(DecodeLimits::kAllocationLimitMb) * 1024ULL * 1024ULL > worstBytes);

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("decode-limits.pure: all checks passed\n");
    return 0;
}
