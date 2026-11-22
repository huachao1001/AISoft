// Windows 文本引擎：GDI 字体 + GetGlyphOutline 抗锯齿字形
//
// 为什么自己栅格化而不是用 ExtTextOut 直接画到窗口：
//   我们的绘制入口是 PaintBackend（一个与设备无关的抽象），目标可能是屏幕
//   DIB，也可能是离线的 Surface。ExtTextOut 只能画进 HDC，一旦用它，文字就
//   绑死在 GDI 表面上，「同一棵控件树渲染到内存」这条能力随即消失。
//   GetGlyphOutline 给我们的是**覆盖率矩阵**，正好能喂给
//   PaintBackend::blendCoverageMask —— 屏幕上和内存里走的是同一条路径。
//
// 为什么用 GGO_GRAY8_BITMAP 而不是 GGO_BITMAP：
//   GGO_BITMAP 只有 0/1 两级，9pt 的中文会糊成锯齿。GRAY8 给 65 级灰度，
//   我们自己把它扩到 0..255，边缘的过渡与直接 ExtTextOut 的抗锯齿一致。
//
// 铁律七（四处同源）在这里的落点：
//   measure / advance / glyphBounds / drawRun 全部由**同一个** pen 推进循环
//   计算，步进取自 GLYPHMETRICS.gmCellIncX。我们不调用
//   GetTextExtentPoint32 之类的「另一个度量入口」——那正是让光标与文字错开
//   半像素的根源。
#pragma once

#include "pyui/text_renderer.h"

#include <string>

namespace pyui {

class GdiTextRenderer final : public TextRenderer {
 public:
  GdiTextRenderer();
  ~GdiTextRenderer() override;

  Font createFont(const FontSpec& spec) override;
  void destroyFont(const Font& font) override;

  TextMetrics measure(std::string_view text, const Font& font) override;
  int advance(std::string_view text, const Font& font) override;
  std::vector<int> glyphBounds(std::string_view text, const Font& font) override;

  void draw(PaintBackend& backend, const Rect& rc, std::string_view text, Color color,
            const Font& font, TextAlign align) override;
  void drawRun(PaintBackend& backend, const Rect& rc, std::string_view text, Color color,
               const Font& font) override;

  const char* defaultFamily() const override;

  // 字形缓存占用的字节数（诊断用：确认缓存没有无限增长）
  size_t cachedGlyphBytes(const Font& font) const;

 private:
  // 平台默认界面字体名。首次调用时从系统设置取，之后缓存。
  std::string defaultFamily_;
};

}  // namespace pyui
