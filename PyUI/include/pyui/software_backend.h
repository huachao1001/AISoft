// 软件光栅器：三平台共用的一份 PaintBackend 实现
//
// 它存在的三个理由：
//
// 1. **离线渲染**。截图、缩略图、打印、CI 像素测试都不该依赖真实窗口。
// 2. **像素级跨平台一致**。原生后端（GDI/GDI+/CoreGraphics/Cairo）的抗锯齿
//    算法各不相同，同一份设计稿必然有几十分之一的像素差异。软件光栅器是
//    唯一能让三平台输出**逐位相同**像素的方式。
// 3. **后端基线**。原生后端出问题时，用它渲染同一棵树，差异立刻能定位到
//    「是控件画错了」还是「后端实现了不同」。
//
// 性能取向：span 填充 + 逐行 memset/memcpy，不追求 GPU 级吞吐，但足以
// 支撑 1080p 60fps 的界面重绘（实测见 DESIGN.md）。屏幕绘制优先走原生后端。
#pragma once

#include "pyui/painter.h"
#include "pyui/surface.h"
#include "pyui/text_renderer.h"

#include <memory>
#include <vector>

namespace pyui {

class SoftwareBackend : public PaintBackend {
 public:
  struct Options {
    // 是否做抗锯齿。关掉用于像素测试的「硬边」基线。
    bool antialias = true;
    // 圆角覆盖率采样：每个像素每轴采样数。1 = 无 AA，4 = 4x4 子采样。
    int samplesPerAxis = 4;
    // 无文本渲染器时 drawText 怎么画：true = 画占位块（测试可预期），
    // false = 完全不画。绝不回退到「什么都不显示但也不报错」的模糊状态。
    bool placeholderText = true;
  };

  SoftwareBackend(Surface& target, Options options = {});
  ~SoftwareBackend() override;

  // 文本渲染器：为空时 drawText 画占位块
  void setTextRenderer(TextRenderer* renderer) { textRenderer_ = renderer; }
  TextRenderer* textRenderer() const { return textRenderer_; }
  // 绑定字体来源：不设则 drawText 无法用真实字体
  void setDefaultFont(Font font) { defaultFont_ = font; }

  Surface& target() { return target_; }

  // ── PaintBackend ──
  void fillRect(const Rect& rc, Color color) override;
  void fillRoundRect(const Rect& rc, Color color, int radius) override;
  void strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth) override;
  void drawGradientRect(const Rect& rc, Color top, Color bottom) override;
  void drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) override;
  void fillCheckerboard(const Rect& rc, Color colorA, Color colorB, int cell) override;
  void blendCoverageMask(const Rect& rc, const std::uint8_t* coverage, int stride,
                         Color color) override;
  void drawText(const Rect& rc, std::string_view text, Color color, const Font& font,
                TextAlign align) override;
  void drawTextRun(const Rect& rc, std::string_view text, Color color, const Font& font) override;
  int measureText(std::string_view text, const Font& font) override;
  int advanceText(std::string_view text, const Font& font) override;
  std::vector<int> glyphBounds(std::string_view text, const Font& font) override;
  int textHeight(const Font& font) override;
  int lineHeight(const Font& font) override;
  void drawIcon(IconId id, const Rect& rc, Color color) override;
  void drawImage(const Image& image, const Rect& rc) override;
  void drawImage(const Image& image, const Rect& rc, const Rect& source) override;
  void drawImageScaled(const Image& image, const Rect& rc, const Rect& source,
                       ImageSmoothing smoothing, float alpha) override;
  void setClip(const Rect& rc) override;
  void resetClip() override;
  void clipRect(const Rect& rc) override;
  void clipRoundRect(const Rect& rc, int radius) override;
  void translate(int dx, int dy) override;
  void rotate(float degrees) override;
  void save() override;
  void restore() override;
  Rect currentClip() const override;

  // ── 诊断 ──
  // 被裁剪丢弃的图元数量。用于验证「脏区外没有多余绘制」。
  int culledPrimitives() const { return culled_; }
  void resetStats() { culled_ = 0; }
  // 当前后端不支持、被静默跳过的图元数量（旋转、位图）。
  // 单独计数而不是混进 culled_：两者原因不同，混在一起会让
  // 「脏区外没有多画」这条验收失效。
  int unsupportedPrimitives() const { return unsupported_; }

 private:
  struct State {
    int tx = 0;
    int ty = 0;
    Rect clip{};
    // 圆角裁剪的半径；-1 表示矩形裁剪
    int clipRadius = -1;
    float rotation = 0.0f;
    bool clipValid = false;
  };

  // 世界坐标 → 表面坐标
  Rect toSurface(const Rect& rc) const;
  // 当前状态下逐行遍历可见区域；回调收到表面坐标的行区间
  template <typename Fn>
  void forEachSpan(const Rect& rc, Fn&& fn);

  void blendPixel(int x, int y, Color color);
  void blendRow(int x0, int x1, int y, Color color, std::uint8_t coverage);
  // 圆角覆盖率（0..255），x/y 是表面坐标
  std::uint8_t roundCornerCoverage(int px, int py, const Rect& surfaceRect, float radius) const;
  // 裁剪覆盖率：矩形裁剪返回 0 或 255，圆角裁剪按采样返回中间值
  std::uint8_t clipCoverage(int x, int y) const;
  void fillRoundRectInternal(const Rect& rc, Color color, float radius);
  void strokeRoundRectInternal(const Rect& rc, Color color, float radius, int lineWidth);
  // 圆角矩形内部的采样覆盖率：用于填充与描边共用同一套边界
  std::uint8_t roundRectCoverage(int x, int y, const Rect& surfaceRect, float radius) const;
  // 一条渐变填充：top/bottom 是同一条竖线上的两端色
  void fillGradientSpan(const Rect& rc, Color top, Color bottom, float radius);
  // 把任意粗细的线段画成轴对齐的矩形集合（图标用，不做抗锯齿）
  void fillThickLine(int x0, int y0, int x1, int y1, int thickness, Color color);

  Surface& target_;
  Options options_;
  TextRenderer* textRenderer_ = nullptr;
  Font defaultFont_;

  State state_{};
  std::vector<State> stack_;
  int culled_ = 0;
  int unsupported_ = 0;
};

// 把一张 ARGB 表面编码成 PNG（不含压缩，仅用于测试基线存储的极简实现）。
// 生产代码请用平台图像后端；这里只服务于测试基础设施。
std::vector<std::uint8_t> encodeSurfaceRaw(const Surface& surface);

}  // namespace pyui
