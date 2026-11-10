// FrameLayout：子视图按 LayoutParams 的绝对坐标叠放
//
// 与 Android 的 FrameLayout 一致：子视图从左上角逐个叠放，后面的盖住
// 前面的。坐标缺省时按对齐方式（Gravity）贴边或居中。
// 内边距直接复用 ViewGroup 的实现。
#pragma once

#include "AiUI/view_group.h"

namespace AiUI {

class FrameLayout : public ViewGroup {
 public:
  FrameLayout() = default;

  // 追加子视图并参与布局；返回自身便于链式调用
  FrameLayout& addChildView(View* child, const LayoutParams& params = {});

 protected:
  Size onMeasure(const Size& available) override;
  void arrangeChildren(const Rect& content) override;
};

}  // namespace AiUI
