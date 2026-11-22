// Button：圆角描边按钮，支持图标 + 文字
//
// 纯绘制：没有原生 BUTTON 子窗口。焦点框、悬停底色、按下位移都在
// onPaint 里画出来，所以三个平台的外观完全由这份代码决定。
#pragma once

#include "pyui/painter.h"
#include "pyui/theme.h"
#include "pyui/view.h"

#include <functional>
#include <string>

namespace pyui {

// 按钮外观
enum class ButtonStyle {
  kOutline,  // 圆角描边 + 白底（工具条）
  kSubtle,   // 无边框，仅悬停底色
  kFlat,     // 完全平铺
  // 幽灵按钮：任何状态都不带背景，只有内容颜色变化。
  // 叠在彩色区域（进度条、图片）上的图标按钮用这个。
  kGhost,
  // 主行动：强调色填充 + 文字。一个界面里最多放一个，
  // 两个同等分量的实心按钮会让人不知道该点哪个。
  kPrimary,
};

// 图标与文字的相对位置
enum class IconPlacement { kNone, kLeft, kTop, kOnly };

struct ButtonSpec {
  std::string label;
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
  // 禁用态；kNoColor 表示按主题派生
  Color disabledBackground = kNoColor;
  Color disabledInk = kNoColor;
  // 悬停时的鼠标形状；标题栏按钮用箭头才与系统一致
  CursorShape cursor = CursorShape::kHand;
};

class Button : public View {
 public:
  Button() { setFocusable(true); }

  void setSpec(const ButtonSpec& spec);
  const ButtonSpec& spec() const { return spec_; }

  void setLabel(std::string label);
  const std::string& label() const { return spec_.label; }
  void setIcon(IconId icon);
  void setStyle(ButtonStyle style);
  void setEnabled(bool enabled) override;

  // 以下四项都直接改 spec 里的对应字段并触发重排/重绘。
  // 分成独立 setter 是因为 XML 属性是逐条生成的，不能要求布局里
  // 一次性给出整个 ButtonSpec。
  void setIconPlacement(IconPlacement placement);
  void setIconSize(int designPixels);
  void setPadX(int designPixels);
  void setRadius(int designPixels);

  // 内容自适应宽度（物理像素）
  int preferredWidth() const;

  CursorShape cursorShape() const override { return spec_.cursor; }

  const char* className() const override { return "Button"; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;

  // 按状态解析出背景、边框、墨色。子类可覆盖以加自定义态。
  virtual void resolvePalette(Color& background, Color& border, Color& ink) const;

 private:
  ButtonSpec spec_{};
};

}  // namespace pyui
