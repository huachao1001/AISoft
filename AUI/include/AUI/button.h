// 自绘按钮：支持图标、文字、图标位置与多种表面样式
#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "AUI/icon.h"
#include "AUI/theme.h"
#include "AUI/widget.h"

namespace AUI {

class Painter;

// 按钮外观
enum class ButtonStyle {
  kOutline,  // 圆角描边 + 白底（工具条）
  kSubtle,   // 无边框，仅悬停底色（图标按钮）
  kFlat,     // 完全平铺，用于内嵌小按钮
};

// 图标与文字的排布
enum class IconPlacement {
  kNone,  // 只有文字
  kLeft,  // 图标在左，文字在右
  kTop,   // 图标在上，文字在下
  kOnly,  // 只有图标
};

// 颜色缺省哨兵：绘制时按主题取值
constexpr COLORREF kUseTheme = CLR_INVALID;

struct ButtonSpec {
  std::wstring label;
  IconId icon = IconId::kFile;
  bool hasIcon = false;
  bool hasText = true;
  IconPlacement placement = IconPlacement::kLeft;
  ButtonStyle style = ButtonStyle::kOutline;

  // 尺寸（96 DPI 设计值）；width/height 为 0 表示按内容自适应
  int width = 0;
  int height = 0;
  int iconSize = 18;
  int padX = 12;
  int iconGap = 6;
  int radius = 7;

  COLORREF bg = kUseTheme;
  COLORREF border = kUseTheme;
  COLORREF ink = kUseTheme;
};

// 独立按钮控件：自己负责布局、悬停/按下、命中与回调
class Button : public Widget {
 public:
  using Handler = std::function<void()>;

  void SetSpec(const ButtonSpec& spec);
  const ButtonSpec& spec() const { return spec_; }
  void SetLabel(const std::wstring& label);
  void SetIcon(IconId icon);
  void SetEnabled(bool enabled);
  bool enabled() const { return enabled_; }
  void SetHandler(Handler fn) { handler_ = std::move(fn); }

  // 内容自适应宽度（含左右内边距）
  int PreferredWidth();

 protected:
  void OnSize(int cx, int cy) override;
  void OnPaint() override;
  bool OnEraseBkgnd(HDC dc) override;
  LRESULT OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) override;

 private:
  // 拆出图标区与文字区；两者都可能为空
  void LayoutParts(Painter& p, const RECT& rc, RECT& iconRc, RECT& textRc, bool& showIcon,
                   bool& showText);
  void PaintButton(Painter& p, const RECT& rc);

  ButtonSpec spec_{};
  bool enabled_ = true;
  bool hot_ = false;
  bool pressed_ = false;
  bool tracking_ = false;
  Handler handler_;
};

}  // namespace AUI
