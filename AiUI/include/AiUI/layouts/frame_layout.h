// FrameLayout：子控件按 LayoutParams 的绝对坐标叠放
//
// 与 Android 的 FrameLayout 一致：子控件从左上角逐个叠放，后面的盖住
// 前面的。坐标缺省时按对齐方式（Gravity）贴边或居中。
#pragma once

#include "AiUI/widget.h"

namespace AiUI {

class FrameLayout : public Widget {
 public:
  FrameLayout() { className_ = L"AiUIFrameLayout"; }

  // 追加子控件并参与布局
  FrameLayout& add(Widget* child, const LayoutParams& params = {});

  // 容器内边距（96 DPI 设计值）
  void setPadding(int all);
  void setPadding(int horizontal, int vertical);
  void setPadding(int left, int top, int right, int bottom);
  const Insets& padding() const { return padding_; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  void arrangeChildren(const Rect& content) override;

 private:
  Insets padding_{};
};

}  // namespace AiUI
