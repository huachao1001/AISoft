// ScrollView：一切可滚动视图的基类，滚动条自绘
//
// 不再使用系统 WS_VSCROLL：整棵树共用一个窗口，滚动条必须自己画。
// 子类只需实现 contentHeight()，并用 scrollOffset() 决定内容从哪里画。
//
// 滚动条是浮层：平时不可见，鼠标进入视图淡出显示，离开后延时隐藏；
// 鼠标停在滚动条上时加宽。因此它不占内容宽度，只按内容是否溢出来决定
// resizeRect 之类的避让。
#pragma once

#include "AiUI/theme.h"
#include "AiUI/view.h"

#include <algorithm>

namespace AiUI {

// 滚动条外观（96 DPI 设计值）
struct ScrollBarStyle {
  // 悬停时轨道与滑块的宽度；平时用窄的那一档，鼠标靠近才展开
  int width = 10;
  int thumbWidth = 6;
  int hoverWidth = 14;
  int hoverThumbWidth = 10;
  int minThumbHeight = 28;
  int margin = 3;  // 与右边缘的距离
  // 两端是圆的，半径取宽度的一半；这里只留开关，便于将来改成直角
  bool rounded = true;
  Color track = kNoColor;  // kNoColor 表示不画轨道
  Color thumb = makeColor(0xc4, 0xc8, 0xd0);
  Color thumbHover = makeColor(0xa8, 0xae, 0xb8);
  // 鼠标离开多久后隐藏；0 表示不自动隐藏
  int hideDelayMs = 3000;
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
  // 内容是否溢出：不溢出就完全不用滚动条
  bool isScrollable() const { return maxScroll() > 0; }
  // 内容要避让的右边距；滚动条是浮层，只在可见时才让位
  int scrollBarReserve() const;
  // 滚动条此刻是否真的画出来（鼠标在视图内或还没到隐藏时间）
  bool scrollBarVisible() const;

  void setScrollOffset(int offset);
  void scrollBy(int delta);
  void scrollToTop() { setScrollOffset(0); }
  void scrollToBottom() { setScrollOffset(maxScroll()); }

 protected:
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
  void onDelayed() override;

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
  // 鼠标进入视图：显示滚动条并重新计时
  void revealScrollBar();
  // 是否处于"显示"状态；由进入/离开与延时共同决定
  void setScrollBarShown(bool shown);
  // 鼠标是否落在滚动条的交互范围内
  bool hitScrollBar(Point local) const;
  // 只重画滚动条那条竖带；宽度切换时新旧两档都要覆盖
  void invalidateBarArea();

  bool draggingThumb_ = false;
  bool thumbHot_ = false;   // 鼠标停在滑块上
  bool trackHot_ = false;   // 鼠标停在轨道上（含滑块）
  bool mouseInside_ = false;  // 鼠标是否还在视图内
  bool barShown_ = false;   // 当前是否绘制滚动条
  // 按下点到滑块顶端的距离，拖动时保持不变
  int dragGrab_ = 0;
};

}  // namespace AiUI
