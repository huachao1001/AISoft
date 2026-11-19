// SidePanel：可整体收起/展开的侧栏容器
//
//   SidePanel (ViewGroup)
//   └── content   调用方塞进来的主体（缩略图条等）
//
// 控件自己不带任何按钮：收起/展开的入口由宿主放在别处（AiPic 放在状态栏
// 最左边）。按钮若留在侧栏里，收窄之后就得在一条缝里塞一个孤零零的图标，
// 既不好看也不好点；放到状态栏后入口永远在同一个位置，收起来时侧栏直接
// 让出整块宽度。
//
// 折叠不是「一下子消失」：宽度逐帧收敛到 0，内容跟着被裁掉。动画只改宽度、
// 不改可见性——中途隐藏内容会让画面空一阵，反而更跳。
#pragma once

#include "AiUI/view_group.h"

#include <functional>

namespace AiUI {

// 侧栏收起/展开的开关按钮：只有一个箭头，方向表示「下一步会怎样」
//
// 单独成控件而不是塞进 SidePanel：它在状态栏里，生命周期与布局归属都跟
// 侧栏不同，硬绑在一起会让两边都别扭。
struct PanelToggleStyle {
  int iconSize = 16;
  // kNoColor 表示跟随主题
  Color icon = kNoColor;
  Color iconDisabled = kNoColor;
  Color hoverBackground = kNoColor;
  Color downBackground = kNoColor;
  // 顶上压一条分隔线，与旁边的状态栏连成一条
  bool topDivider = true;
  Color divider = kNoColor;
};

class PanelToggle : public View {
 public:
  PanelToggle() { setClaimsPointer(true); }

  void setStyle(const PanelToggleStyle& style);
  const PanelToggleStyle& style() const { return style_; }

  // 侧栏当前是否收着：箭头方向跟着变，展开时指左（收起）、收起时指右。
  // 设了状态源就以它为准——箭头永远反映真实状态，不必要求每条改动路径都
  // 记得手动同步一次。
  using CollapsedQuery = std::function<bool()>;
  void setCollapsedSource(CollapsedQuery fn) { collapsedSource_ = std::move(fn); }
  void setCollapsed(bool collapsed);
  bool isCollapsed() const;

  using ToggleHandler = std::function<void()>;
  void setOnToggle(ToggleHandler fn) { onToggle_ = std::move(fn); }

  // 图标矩形（局部坐标），供测试与命中判断复用
  Rect iconRect() const;

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  Color backgroundColor() const override;

 private:
  PanelToggleStyle style_{};
  bool collapsed_ = false;
  bool hot_ = false;
  bool pressed_ = false;
  ToggleHandler onToggle_;
  CollapsedQuery collapsedSource_;
};

class SidePanel : public ViewGroup {
 public:
  SidePanel() = default;
  ~SidePanel() override;

  // 接管 body 的所有权
  void setContent(View* body);
  View* content() const { return body_; }

  // 侧栏展开时的宽度（96 DPI 设计值）。收起后是 0
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
  void applyWidth(int designWidth);
  int targetWidth() const;

  View* body_ = nullptr;
  int expandedWidth_ = 208;
  bool collapsed_ = false;
  bool animating_ = false;
  // 动画起止宽度（设计值）
  int animFrom_ = 0;
  int animTo_ = 0;
  int64_t animStartMs_ = 0;
  int animDurationMs_ = 160;
  // 上一帧已下发的宽度：相同就不必再次改布局参数
  int lastAppliedWidth_ = -1;
  ToggleHandler onCollapsedChanged_;
};

}  // namespace AiUI
