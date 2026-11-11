// ScrollView：一切可滚动视图的基类，滚动条自绘
//
// 不再使用系统 WS_VSCROLL / WS_HSCROLL：整棵树共用一个窗口，滚动条必须自己画。
// 子类实现 contentHeight() / contentWidth()，并用 scrollOffset() /
// scrollOffsetX() 决定内容从哪里画。内容不溢出对应方向时，该方向不出现滚动条。
//
// 两个方向默认都开启，子类或使用者可以单独关掉；关掉后该方向既不滚动也不画条。
//
// 滚动条是浮层：平时不可见，鼠标进入视图才出现，离开后延时隐藏。只有鼠标
// 停在滑块上时滑块才加粗，停在轨道上保持不变，避免整条条子乱跳。
#pragma once

#include "AiUI/theme.h"
#include "AiUI/view.h"

#include <algorithm>

namespace AiUI {

// 滚动条外观（96 DPI 设计值）。水平与垂直共用同一套厚度，只是方向不同。
struct ScrollBarStyle {
  // 轨道厚度；0 表示不画轨道
  int width = 10;
  // 滑块平时的厚度
  int thumbWidth = 6;
  // 鼠标停在滑块上时的厚度；只比平时略粗，太粗会盖住内容
  int hotThumbWidth = 9;
  // 滑块长度下限，太短的滑块不好拖
  int minThumbLength = 28;
  int margin = 3;  // 与视图边缘的距离
  // 两端是圆的，半径取厚度的一半；留开关便于将来改成直角
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

  // ── 方向开关：默认两个方向都开启 ──
  void setVerticalScrollEnabled(bool enabled);
  void setHorizontalScrollEnabled(bool enabled);
  bool isVerticalScrollEnabled() const { return verticalEnabled_; }
  bool isHorizontalScrollEnabled() const { return horizontalEnabled_; }

  // ── 滚动状态 ──
  int scrollOffset() const { return scroll_; }
  int scrollOffsetX() const { return scrollX_; }

  // 内容总尺寸（物理像素）；子类按行数 × 行高、最长文本等计算
  virtual int contentHeight() const = 0;
  // 默认不横向溢出：只有真正有宽内容的子类才需要覆写
  virtual int contentWidth() const { return viewportWidth(); }

  // 视口高度：扣掉表头之类固定区域
  virtual int viewportHeight() const { return clientRect().height(); }
  // 视口顶边：有固定表头的视图把滚动条整体下移
  virtual int viewportTop() const { return 0; }
  virtual int viewportWidth() const { return clientRect().width(); }
  virtual int viewportLeft() const { return 0; }

  // 可滚动的最大偏移
  int maxScroll() const { return (std::max)(0, contentHeight() - viewportHeight()); }
  int maxScrollX() const { return (std::max)(0, contentWidth() - viewportWidth()); }
  // 该方向是否真的有内容溢出
  bool isScrollable() const { return verticalEnabled_ && maxScroll() > 0; }
  bool isScrollableX() const { return horizontalEnabled_ && maxScrollX() > 0; }

  // 内容要避让的右边距；滚动条是浮层，只在纵向可滚动时才让位
  int scrollBarReserve() const;
  // 滚动条此刻是否真的画出来（鼠标在视图内或还没到隐藏时间）
  bool scrollBarVisible() const;
  bool hScrollBarVisible() const;

  // 滚动条几何：垂直条贴右边，水平条贴底边；两者相遇处互相让位
  Rect vTrackRect() const;
  Rect hTrackRect() const;
  Rect vThumbRect() const;
  Rect hThumbRect() const;

  void setScrollOffset(int offset);
  void scrollBy(int delta);
  void setScrollOffsetX(int offset);
  void scrollByX(int delta);
  void scrollToTop() { setScrollOffset(0); }
  void scrollToBottom() { setScrollOffset(maxScroll()); }
  void scrollToLeft() { setScrollOffsetX(0); }
  void scrollToRight() { setScrollOffsetX(maxScrollX()); }

 protected:
  // 滚动条可用区域：视口范围，不含固定表头
  Rect viewportRect() const;

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
  int scrollX_ = 0;
  // 两个方向默认都开启
  bool verticalEnabled_ = true;
  bool horizontalEnabled_ = true;

 private:
  // 正在拖动哪个方向的滑块
  enum class Bar { kNone, kVertical, kHorizontal };

  bool hitVThumb(Point local) const;
  bool hitHThumb(Point local) const;
  bool hitVTrack(Point local) const;
  bool hitHTrack(Point local) const;
  // 滑块位置对应的滚动偏移
  int scrollForVThumb(int thumbTop) const;
  int scrollForHThumb(int thumbLeft) const;
  int clampScroll(int offset) const;
  int clampScrollX(int offset) const;
  // 鼠标进入视图：显示滚动条
  void revealScrollBar();
  // 是否处于"显示"状态；由进入/离开与延时共同决定
  void setScrollBarShown(bool shown);
  // 鼠标是否落在任一滚动条的交互范围内
  bool hitScrollBar(Point local) const;
  // 只重画滚动条占据的两条边带
  void invalidateBarArea();

  Bar dragging_ = Bar::kNone;
  bool vThumbHot_ = false;   // 鼠标停在垂直滑块上
  bool hThumbHot_ = false;   // 鼠标停在水平滑块上
  bool mouseInside_ = false;  // 鼠标是否还在视图内
  bool barShown_ = false;   // 当前是否绘制滚动条
  // 按下点到滑块起始边的距离，拖动时保持不变
  int dragGrab_ = 0;
};

}  // namespace AiUI
