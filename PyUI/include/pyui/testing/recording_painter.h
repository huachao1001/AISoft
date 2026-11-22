// 记录型画笔：把每一次绘制调用录成可比较的序列
//
// 用途是「绘制命令跨平台必须逐字节相同」这条验收：
//
//   同一个控件 + 同一组数据 + 同一份主题  →  两个平台录出来的命令序列必须相等
//
// 它不做任何光栅化，也不产生像素。像素正确性由 SoftwareBackend 的
// 不变量测试覆盖。两者各管一段，缺一不可：
//   命令一致  → 控件想画什么在每个平台都一样
//   像素不变量 → 画出来的东西形状是对的
#pragma once

#include "pyui/painter.h"

#include <ostream>
#include <string>
#include <vector>

namespace pyui {

// 一条绘制命令。用结构体而不是字符串，比较时可以给出精确的差异位置。
struct DrawCommand {
  enum class Kind {
    kFillRect,
    kFillRoundRect,
    kStrokeRoundRect,
    kGradientRect,
    kGradientRoundRect,
    kCheckerboard,
    kCoverageMask,
    kText,
    kTextRun,
    kIcon,
    kImage,
    kImageScaled,
    kSetClip,
    kResetClip,
    kClipRect,
    kClipRoundRect,
    kTranslate,
    kRotate,
    kSave,
    kRestore,
  };

  Kind kind = Kind::kFillRect;
  Rect rect{};
  Rect rect2{};        // kImageScaled 的 source
  Color color = 0;
  Color color2 = 0;    // 渐变的下端点色
  int radius = 0;
  int lineWidth = 0;
  int intValue = 0;    // 棋盘格 cell / 文本对齐
  int intValue2 = 0;   // translate 的 dy
  float floatValue = 0.0f;  // rotate / alpha
  std::string text;    // 文本内容（不含字体）
  int fontHeight = 0;  // 字号（不用指针，保证可比）
  IconId icon = IconId::kNone;

  // 参与比较的字段全部列在这里。刻意不含字符串化输出，避免格式化差异。
  bool operator==(const DrawCommand& o) const;
  bool operator!=(const DrawCommand& o) const { return !(*this == o); }

  std::string describe() const;
};

class RecordingPainter : public PaintBackend {
 public:
  RecordingPainter() = default;

  const std::vector<DrawCommand>& commands() const { return commands_; }
  void clear() { commands_.clear(); }

  // 两段命令序列的差异描述；完全相同返回空串。
  static std::string diff(const std::vector<DrawCommand>& a, const std::vector<DrawCommand>& b);
  // 人类可读的完整转储（失败时打印用）
  std::string dump() const;
  void dumpTo(std::ostream& os) const;

  // ── PaintBackend ──
  void fillRect(const Rect& rc, Color color) override;
  void fillRoundRect(const Rect& rc, Color color, int radius) override;
  void strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth) override;
  void drawGradientRect(const Rect& rc, Color top, Color bottom) override;
  void drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) override;
  void fillCheckerboard(const Rect& rc, Color colorA, Color colorB, int cell) override;
  // 只记录「在这里用这个颜色打了一块蒙版」，不记录蒙版内容——
  // 蒙版是字形栅格化结果，**必然**因平台字体而异，写进命令序列会让
  // 「跨平台命令逐字节相同」这条验收永远失败。命令层要证明的是
  // 「控件想在哪里放字」，不是「字体长什么样」。
  void blendCoverageMask(const Rect& rc, const std::uint8_t* coverage, int stride,
                         Color color) override;
  void drawText(const Rect& rc, std::string_view text, Color color, const Font& font,
                TextAlign align) override;
  void drawTextRun(const Rect& rc, std::string_view text, Color color, const Font& font) override;
  int measureText(std::string_view text, const Font& font) override;
  int advanceText(std::string_view text, const Font& font) override;
  int textHeight(const Font& font) override;
  void drawIcon(IconId id, const Rect& rc, Color color) override;
  void drawImage(const Image& image, const Rect& rc) override;
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

  // 记录型画笔的度量：固定步进，供不需要真实字体的布局测试使用
  void setMeasureAdvance(int perChar) { measureAdvance_ = perChar; }
  void setMeasureHeight(int height) { measureHeight_ = height; }

 private:
  void push(DrawCommand cmd) { commands_.push_back(std::move(cmd)); }

  std::vector<DrawCommand> commands_;
  int measureAdvance_ = 7;
  int measureHeight_ = 14;
};

}  // namespace pyui
