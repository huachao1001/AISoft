// Button：圆角描边按钮，支持图标 + 文字
#pragma once

#include "AiUI/painter.h"
#include "AiUI/theme.h"
#include "AiUI/view.h"

#include <functional>
#include <string>

namespace AiUI {

// 按钮外观
enum class ButtonStyle {
  kOutline,  // 圆角描边 + 白底（工具条）
  kSubtle,   // 无边框，仅悬停底色
  kFlat,     // 完全平铺
};

// 图标与文字的相对位置
enum class IconPlacement { kNone, kLeft, kTop, kOnly };

struct ButtonSpec {
  std::wstring label;
  IconId icon = IconId::kNone;
  bool hasIcon = false;
  IconPlacement placement = IconPlacement::kLeft;
  ButtonStyle style = ButtonStyle::kOutline;

  // 尺寸（96 DPI 设计值）；0 表示按内容自适应
  int width = 0;
  int height = 32;
  int iconSize = 18;
  int padX = 12;
  int iconGap = 6;
  int radius = 7;

  Color background = kNoColor;
  Color border = kNoColor;
  Color ink = kNoColor;
  // 悬停与按下态：给了显式值就按它们画，否则用主题色
  Color hoverBackground = kNoColor;
  Color downBackground = kNoColor;
  Color hoverInk = kNoColor;
  // 悬停时的鼠标形状；标题栏按钮用箭头才与系统一致
  CursorShape cursor = CursorShape::kHand;
};

class Button : public View {
 public:
  Button() = default;

  void setSpec(const ButtonSpec& spec);
  const ButtonSpec& spec() const { return spec_; }

  void setLabel(const std::wstring& label);
  void setIcon(IconId icon);
  void setEnabled(bool enabled) override;

  // 内容自适应宽度（物理像素）
  int preferredWidth();

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;

 private:
  // 按状态解析出背景、边框、墨色
  void resolvePalette(Color& background, Color& border, Color& ink) const;

  ButtonSpec spec_{};
  bool hot_ = false;
  bool pressed_ = false;
};

}  // namespace AiUI
