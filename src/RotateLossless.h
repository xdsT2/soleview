#pragma once

#include <QString>

// 旋转 / 翻转的「无损方向元数据写回」模块（#6，2026-11-29）。
//
// 与旧 RotateAutoSave 的根本区别：本模块绝不重编码像素——只修改或补写文件里的
// 方向元数据（EXIF Orientation / WebP EXIF chunk / HEIC irot / TIFF IFD0 标签），
// 压缩图像数据逐字节不变；其余元数据原样保留。写回走「同目录临时文件 → flush/fsync
// → 原子替换」，任何一步失败都清理临时文件，原文件保持完好；只读 / 无权限干净失败。
//
// 放行口径（阶段一实测矩阵，见 06-无损写回可行性-20261129.md）：
//   - JPEG：已有方向标签原地改；无标签补 IFD0 条目；无 APP1 段插入 APP1；
//   - WebP：已有 EXIF chunk 原地改；VP8 有损无 EXIF 时新建 VP8X + EXIF chunk；
//   - HEIC：仅主 item 已有 irot、非镜像、纯旋转子集（方向 1/3/6/8）原位改 1 字节；
//   - TIFF：仅单页经典 TIFF（非 BigTIFF、无 IFD 链第二页）。
// 跳过：PNG/BMP/GIF/HDR/EXR/AVIF/RAW/PSD/PSB、动图（GIF/APNG/ANIM/WebP 动画）、
// 多页 TIFF、BigTIFF、HEIC 无 irot / 镜像 / 需改 Exif item、VP8L 无 VP8X、
// 所有未验证能安全改方向元数据的文件。
namespace RotateLossless {

// 三模式（与 Config 持久化枚举镜像，UI 不直接读配置键）。
enum class Mode {
    PreviewOnly = 0,   // 仅预览：不探写回、不出现保存按钮
    Manual = 1,        // 手动：旋转后底栏出独立「无损保存」按钮，点击才写回
    Auto = 2,          // 自动：旋转落地立即尝试写回；成功静默，失败轻提示并保留重试入口
};

// 结构判定结果。supported=false 时 reason 为原因码（供去重与回执矩阵），
// detail 为模块侧收集到的结构细节（不含用户可见文案；toast 由宿主统一翻译）。
struct Probe {
    bool supported = false;
    QString reason;
    QString detail;
};

// 只做结构判定，不读像素、不解码图像。
Probe probe(const QString &path);

// 读取文件记录的 EXIF 方向值（1..8）；无标签 / 未知 / 不支持的容器返回 0。
int readOrientation(const QString &path);

// 纯字节补丁：读原文件 → 内存生成补丁字节 → 同目录临时文件 + fsync + 原子替换。
// newOrientation 为合成后的 EXIF 方向值（1..8）；HEIC 内部自行映射 irot 编码。
// 成功返回 true；失败返回 false 并填 err（可展示原因），原文件完好。绝不重编码。
bool writeOrientation(const QString &path, int newOrientation, QString &err);

// —— D4 方向合成（Pillow Transpose 实证表，锚点已用 UI 探针校准）——
// orig 为文件原方向（1..8；0/未知按 1 处理）。
// 应用一次顺时针 90°（应用内「右转」，m_netRotation +90）。
int composeRotateCw(int orig);
// 应用一次逆时针 90°（应用内「左转」，m_netRotation -90）。
int composeRotateCcw(int orig);
// 应用一次 180° 旋转。
int composeRotate180(int orig);
// 应用一次屏幕水平镜像。
int composeFlipH(int orig);
// 应用一次屏幕垂直镜像。
int composeFlipV(int orig);

}   // namespace RotateLossless
