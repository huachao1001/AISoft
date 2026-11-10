// ScrollView：一切可滚动视图的基类，滚动条自绘
//
// 不再使用系统 WS_VSCROLL：整棵树共用一个窗口，滚动条必须自己画。
// 子类只需实现 contentHeight()，并用 scrollOffset() 决定内容从哪里画。
// 滚动条只在内容超出视口时出现，出现时右侧留出 scrollBarReserve()。
#pragma once

#include "AiUI/theme.h"
#include "AiUI/view.h"

#include <algorithm>

namespace AiUI {

// 滚动条外观（96 DPI 设计值）
struct ScrollBarStyle {
  int width = 10;        // 轨道宽度
  int thumbWidth = 6;    // 滑块宽度
  int minThumbHeight = 28;
  int radius = 3;
  int margin = 3;        // 与右边缘的距离
  Color track = kNoColor;  // kNoColor 表示不画轨道
  Color thumb = makeColor(0xc4, 0xc8, 0xd0);
  Color thumbHover = makeColor(0xa8, 0xae, 0xb8);
};

class ScrollView : public View {
 public:
  ScrollView() = default;

  void setScrollBarStyle(const ScrollBarStyle& style);
  const ScrollBarStyle& style() const { return style_; }

  // ── 滚动状态 ──
  int scrollOffset() const { return scroll_; }
  // 内容总高（物理像素）；子类按行数 × 行高等计算
  virtual int contentHeight() const = 0;
  // 视口高度：扣掉表头之类固定区域
  virtual int viewportHeight() const { return clientRect().height(); }
  // 视口顶边：有固定表头的视图把滚动条整体下移
  virtual int viewportTop() const { return 0; }
  // 可滚动的最大偏移
  int maxScroll() const { return (std::max)(0, contentHeight() - viewportHeight()); }
  // 有滚动条时内容要避让的右边距；没有滚动条返回 0
  int scrollBarReserve() const;

  void setScrollOffset(int offset);
  void scrollBy(int delta);
  void scrollToTop() { setScrollOffset(0); }
  void scrollToBottom() { setScrollOffset(maxScroll()); }

 protected:
  // 供子类在测量/绘制时使用
  bool scrollBarVisible() const;
  // 滚动条可用区域：视口范围，不含固定表头
  Rect viewportRect() const;
  Rect thumbRect() const;
  Rect trackRect() const;

  void onDrawForeground(Painter& painter, const Rect& rect) override;
  bool onMouseWheel(const MouseEvent& e) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  void onLayout(const Rect& frame) override;

  // 滚轮一格对应的像素数（96 DPI 设计值）
  virtual int wheelStep() const { return 60; }
  // 滚动位置变化后回调；默认整块重画，列表可以只重画内容区
  virtual void onScrollChanged() { invalidate(); }

  ScrollBarStyle style_{};
  int scroll_ = 0;

 private:
  bool hitThumb(Point local) const;
  bool hitTrack(Point local) const;
  // 滑块顶端位置对应的滚动偏移
  int scrollForThumbTop(int thumbTop) const;
  // 子类改动滚动位置后统一走这里，保证夹紧与重绘
  int clampScroll(int offset) const;

  bool draggingThumb_ = false;
  bool thumbHot_ = false;
  // 按下点到滑块顶端的距离，拖动时保持不变
  int dragGrab_ = 0;
};

}  // namespace AiUI
