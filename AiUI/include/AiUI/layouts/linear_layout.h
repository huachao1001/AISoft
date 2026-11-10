// LinearLayout：子控件按单一方向依次排列
//
// 与 Android 的 LinearLayout 一致：weight 为 0 的子控件按自身尺寸占据
// 空间，剩余空间按各子控件的 weight 比例分配。横向排列时 weight 拉伸宽
// 度，纵向排列时拉伸高度。
#pragma once

#include "AiUI/widget.h"

namespace AiUI {

enum class Orientation { kHorizontal, kVertical };

class LinearLayout : public Widget {
 public:
  explicit LinearLayout(Orientation orientation = Orientation::kVertical);

  LinearLayout& add(Widget* child, const LayoutParams& params = {});

  void setOrientation(Orientation orientation);
  Orientation orientation() const { return orientation_; }

  // 主轴方向子控件之间的间距（96 DPI 设计值）
  void setSpacing(int spacing);
  int spacing() const { return spacing_; }
  // 交叉轴方向的对齐方式
  void setGravity(Gravity gravity) { gravity_ = gravity; }
  Gravity gravity() const { return gravity_; }

  void setPadding(int all);
  void setPadding(int horizontal, int vertical);
  void setPadding(int left, int top, int right, int bottom);
  const Insets& padding() const { return padding_; }

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  void arrangeChildren(const Rect& content) override;

 private:
  Orientation orientation_ = Orientation::kVertical;
  Gravity gravity_ = Gravity::kStart;
  int spacing_ = 0;
  Insets padding_{};
};

}  // namespace AiUI
