// 旋转保存三模式迁移测试（#6）：旧布尔键 config/auto_save_rotate → 新枚举
// config/rotate_save_mode 的一次性迁移；新装 / false / 无键落「仅预览」，
// true 落「自动保存」；迁移完成后以新键为准。
#include "Config.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            ++g_failures; \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static Config loadAt(const QString &path)
{
    return Config(path);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // A. 旧版已开自动保存（旧键 true）→ 迁入自动保存(2)；旧键保留供降级回退。
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QString ini = dir.path() + QStringLiteral("/a.ini");
        {
            QSettings s(ini, QSettings::IniFormat);
            s.setValue(QStringLiteral("config/auto_save_rotate"), true);
            s.sync();
        }
        Config cfg = loadAt(ini);
        CHECK(cfg.rotateSaveMode() == 2);
        QSettings s(ini, QSettings::IniFormat);
        CHECK(s.contains(QStringLiteral("config/auto_save_rotate")));   // 旧键保留
        CHECK(s.value(QStringLiteral("config/rotate_save_mode")).toInt() == 2);
        CHECK(s.value(QStringLiteral("config/etc/rotate_save_mode_migrated")).toBool());
    }

    // B. 旧键 false → 仅预览(0)。
    {
        QTemporaryDir dir;
        const QString ini = dir.path() + QStringLiteral("/b.ini");
        {
            QSettings s(ini, QSettings::IniFormat);
            s.setValue(QStringLiteral("config/auto_save_rotate"), false);
            s.sync();
        }
        Config cfg = loadAt(ini);
        CHECK(cfg.rotateSaveMode() == 0);
    }

    // C. 新装：无旧键 → 仅预览(0)。
    {
        QTemporaryDir dir;
        const QString ini = dir.path() + QStringLiteral("/c.ini");
        Config cfg = loadAt(ini);
        CHECK(cfg.rotateSaveMode() == 0);
    }

    // D. 迁移完成后以新键为准：手动改到 1（手动保存），重开读取仍是 1。
    {
        QTemporaryDir dir;
        const QString ini = dir.path() + QStringLiteral("/d.ini");
        {
            Config cfg = loadAt(ini);
            cfg.setRotateSaveMode(1);
        }
        Config again = loadAt(ini);
        CHECK(again.rotateSaveMode() == 1);
        QSettings s(ini, QSettings::IniFormat);
        CHECK(s.value(QStringLiteral("config/rotate_save_mode")).toInt() == 1);
    }

    return g_failures == 0 ? 0 : 1;
}
