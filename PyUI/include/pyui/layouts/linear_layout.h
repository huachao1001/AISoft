// 线性布局：横向或纵向依次排列，支持 weight 分配剩余空间
#pragma once

#include "pyui/view_group.h"

namespace pyui {

enum class Orientation { kHorizontal, kVertical };

class LinearLayout : public ViewGroup {
 public:
  LinearLayout() = default;
  explicit LinearLayout(Orientation orientation) : orientation_(orientation) {}

  void setOrientation(Orientation orientation);
  Orientation orientation() const { return orientation_; }

  // 主轴上的对齐（整体内容在容器内的位置）
  void setGravity(Gravity gravity) { gravity_ = gravity; }
  Gravity gravity() const { return gravity_; }

  // 子视图间距（96 DPI 设计值），只加在相邻子视图之间
  void setSpacing(int spacing) { spacing_ = spacing; }
  int spacing() const { return spacing_; }

  const char* className() const override { return "LinearLayout"; }

 protected:
  void arrangeChildren(const Rect& content) override;
  Size onMeasure(const Size& available) override;

 private:
  Orientation orientation_ = Orientation::kVertical;
  Gravity gravity_ = Gravity::kStart;
  int spacing_ = 0;
};

}  // namespace pyui
