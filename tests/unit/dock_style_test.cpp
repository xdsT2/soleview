// DOCK-2 底栏三档样式（Floating / Capsule / Edge）映射与几何/档位单测。
// 覆盖：配置键 ↔ 枚举互转与未知值回落；宽档轨道长度、贴底满宽与滑条上限；
// 窄档不超可用宽、轨道收起；宽档不常驻页码。离屏 QApplication，不依赖真实光标。
#include "Chrome.h"

#include <QApplication>
#include <QString>
#include <QVector>

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            ++g_failures; \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

using Style = Chrome::ControlBar::Style;

static bool hasHidden(const QVector<int> &ids, int id)
{
    return ids.contains(id);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);

    // —— 1. 配置键 ↔ 枚举互转；未知 / 空串一律回落 Floating ——
    CHECK(Chrome::ControlBar::styleFromString(QStringLiteral("floating")) == Style::Floating);
    CHECK(Chrome::ControlBar::styleFromString(QStringLiteral("capsule")) == Style::Capsule);
    CHECK(Chrome::ControlBar::styleFromString(QStringLiteral("edge")) == Style::Edge);
    CHECK(Chrome::ControlBar::styleFromString(QStringLiteral("weird")) == Style::Floating);
    CHECK(Chrome::ControlBar::styleFromString(QString()) == Style::Floating);

    CHECK(Chrome::ControlBar::styleKey(Style::Floating) == QStringLiteral("floating"));
    CHECK(Chrome::ControlBar::styleKey(Style::Capsule) == QStringLiteral("capsule"));
    CHECK(Chrome::ControlBar::styleKey(Style::Edge) == QStringLiteral("edge"));
    // 往返一致性
    CHECK(Chrome::ControlBar::styleKey(
              Chrome::ControlBar::styleFromString(QStringLiteral("capsule")))
          == QStringLiteral("capsule"));

    // —— 2. 三样式宽档几何（可用宽 1600，目录 10 张、当前第 3 张）——
    struct Case {
        Style style;
        const char *name;
        int expectTrack;      // 悬浮两档轨道定稿宽；Edge 只校验区间
        bool filledWidth;     // Edge 宽档背景铺满可用宽
    };
    const Case cases[] = {
        { Style::Floating, "floating", 120, false },
        { Style::Capsule,  "capsule",  240, false },
        { Style::Edge,     "edge",       0, true  },
    };

    for (const Case &c : cases) {
        Chrome::ControlBar bar;
        bar.setStyle(c.style);
        bar.setPage(3, 10);
        CHECK(bar.style() == c.style);

        // 宽档：进宽档、带滑条、不超出可用宽。
        bar.setAvailableWidth(1600);
        CHECK(bar.tierLevel() == 0);
        CHECK(bar.trackWidth() > 0);
        CHECK(bar.width() <= 1600);
        // 宽档全部动作上岛：滑条(9)、页码(10)都不在「更多」隐藏表里。
        CHECK(!hasHidden(bar.hiddenActionIds(), 9));
        CHECK(!hasHidden(bar.hiddenActionIds(), 10));

        if (c.filledWidth) {
            // 贴底宽档：背景铺满窗口宽度；滑条有上限，最大化时两侧留白。
            CHECK(bar.width() == 1600);
            CHECK(bar.trackWidth() <= 320);
            CHECK(bar.trackWidth() >= 120);
            // 最大化更宽窗口：轨道被上限卡住，不再随窗口拉伸到边。
            bar.setAvailableWidth(2560);
            CHECK(bar.tierLevel() == 0);
            CHECK(bar.width() == 2560);
            CHECK(bar.trackWidth() == 320);
        } else {
            // 两套悬浮：自然宽岛，不满铺；轨道长度按各样式定稿。
            CHECK(bar.width() < 1600);
            CHECK(bar.trackWidth() == c.expectTrack);
        }

        // 窄窗：轨道收起（中/窄档无滑条），控件宽不得压过可用宽，按钮热区保留。
        bar.setAvailableWidth(240);
        CHECK(bar.trackWidth() == 0);
        CHECK(bar.width() <= 240);
        CHECK(bar.preferredWidth() <= 240 || bar.width() <= 240);
    }

    // —— 3. 单张图：滑条禁用但几何不崩；多图目录宽档轨道在位 ——
    {
        Chrome::ControlBar bar;
        bar.setStyle(Style::Edge);
        bar.setPage(1, 1);
        bar.setAvailableWidth(1600);
        CHECK(bar.tierLevel() == 0);
        CHECK(bar.width() == 1600);

        bar.setPage(1, 500);
        bar.setAvailableWidth(1600);
        CHECK(bar.trackWidth() > 0);
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "dockstyle.pure: %d 项断言失败\n", g_failures);
        return 1;
    }
    std::printf("dockstyle.pure: OK\n");
    return 0;
}
