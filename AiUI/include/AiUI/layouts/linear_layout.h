// LinearLayout：子视图按单一方向依次排列
//
// 与 Android 的 LinearLayout 一致：weight 为 0 的子视图按自身尺寸占据
// 空间，剩余空间按各子视图的 weight 比例分配。横向排列时 weight 拉伸宽
// 度，纵向排列时拉伸高度。
#pragma once

#include "AiUI/view_group.h"

namespace AiUI {

enum class Orientation { kHorizontal, kVertical };

class LinearLayout : public ViewGroup {
 public:
  explicit LinearLayout(Orientation orientation = Orientation::kVertical);

  // 追加子视图并参与布局；返回自身便于链式调用
  LinearLayout& addChildView(View* child, const LayoutParams& params = {});

  void setOrientation(Orientation orientation);
  Orientation orientation() const { return orientation_; }

  // 主轴方向子视图之间的间距（96 DPI 设计值）
  void setSpacing(int spacing);
  int spacing() const { return spacing_; }
  // 交叉轴方向的对齐方式
  void setGravity(Gravity gravity) {
    gravity_ = gravity;
    invalidate();
    requestLayout();
  }
  Gravity gravity() const { return gravity_; }

 protected:
  Size onMeasure(const Size& available) override;
  void arrangeChildren(const Rect& content) override;

 private:
  Orientation orientation_ = Orientation::kVertical;
  Gravity gravity_ = Gravity::kStart;
  int spacing_ = 0;
};

}  // namespace AiUI
