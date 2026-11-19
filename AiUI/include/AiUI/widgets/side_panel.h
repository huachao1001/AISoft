// SidePanel：带可收缩顶栏的侧栏容器
//
//   SidePanel (ViewGroup)
//   ├── header : PanelHeader   目录名 + 收缩按钮；折叠后变竖排
//   └── content                 调用方塞进来的主体（缩略图条等）
//
// 折叠不是「一下子消失」：宽度逐帧收敛到只剩一条窄边，内容跟着被裁掉。
// 动画只改宽度，不改可见性——中途隐藏内容会让画面空一阵，反而更跳。
#pragma once

#include "AiUI/view_group.h"

#include <functional>
#include <string>

namespace AiUI {

// 顶栏外观（96 DPI 设计值）
struct PanelHeaderStyle {
  int height = 34;
  int padLeft = 12;
  int padRight = 8;
  // 折叠后那条窄边的宽度：只放得下一个图标和竖排文字
  int collapsedWidth = 30;
  int titleGap = 8;
  // kNoColor 表示跟随主题
  Color background = kNoColor;
  Color text = kNoColor;
  Color buttonHover = kNoColor;
};

class PanelHeader : public View {
 public:
  PanelHeader() { setClaimsPointer(true); }

  void setText(const std::wstring& text);
  const std::wstring& text() const { return text_; }
  // 完整路径：目录名会被省略，靠悬停提示补全
  void setHint(const std::wstring& hint);

  void setStyle(const PanelHeaderStyle& style);
  const PanelHeaderStyle& style() const { return style_; }

  // 折叠态下标题竖排，按钮改成「展开」方向
  void setCollapsed(bool collapsed);
  bool isCollapsed() const { return collapsed_; }
  // 当前宽度是否已窄到放不下横排标题
  bool isNarrow() const;

  // 收缩/展开按钮的点击
  using ToggleHandler = std::function<void()>;
  void setOnToggle(ToggleHandler fn) { onToggle_ = std::move(fn); }

  // 按钮矩形（局部坐标），供测试与将来加按钮时复用
  Rect toggleRect() const;

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  Color backgroundColor() const override;

 private:
  Rect localToggleRect() const;

  std::wstring text_;
  PanelHeaderStyle style_{};
  bool collapsed_ = false;
  bool hot_ = false;
  bool pressed_ = false;
  ToggleHandler onToggle_;
};

class SidePanel : public ViewGroup {
 public:
  SidePanel() { ensureHeader(); }
  ~SidePanel() override;

  // 接管 body 的所有权；顶栏由控件自己建
  void setContent(View* body);
  View* content() const { return body_; }

  PanelHeader* header() const { return header_; }

  // 侧栏展开时的宽度（96 DPI 设计值）。折叠后收敛到顶栏的窄边宽度
  void setExpandedWidth(int width);
  int expandedWidth() const { return expandedWidth_; }

  void setCollapsed(bool collapsed, bool animated = true);
  bool isCollapsed() const { return collapsed_; }
  bool isAnimating() const { return animating_; }

  using ToggleHandler = std::function<void()>;
  // 动画结束（或立即切换完成）后回调，宿主据此做后续布局调整
  void setOnCollapsedChanged(ToggleHandler fn) { onCollapsedChanged_ = std::move(fn); }

 protected:
  Size onMeasure(const Size& available) override;
  void onLayout(const Rect& frame) override;
  void onDelayed() override;

 private:
  void ensureHeader();
  void applyWidth(int designWidth);
  int targetWidth() const;

  PanelHeader* header_ = nullptr;
  View* body_ = nullptr;
  int expandedWidth_ = 208;
  bool collapsed_ = false;
  bool animating_ = false;
  // 动画起止宽度（物理像素）
  int animFrom_ = 0;
  int animTo_ = 0;
  int64_t animStartMs_ = 0;
  int animDurationMs_ = 160;
  // 上一帧已下发的宽度：相同就不必再次改布局参数
  int lastAppliedWidth_ = -1;
  ToggleHandler onCollapsedChanged_;
};

}  // namespace AiUI
