// 帧布局：子视图按绝对坐标叠放，后添加的盖在上面
#pragma once

#include "pyui/view_group.h"

namespace pyui {

class FrameLayout : public ViewGroup {
 public:
  FrameLayout() = default;

  const char* className() const override { return "FrameLayout"; }

 protected:
  void arrangeChildren(const Rect& content) override;
  Size onMeasure(const Size& available) override;
};

}  // namespace pyui
