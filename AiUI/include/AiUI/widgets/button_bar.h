// ButtonBar：横向按钮条，支持分组与分组间的竖分隔线
#pragma once

#include "AiUI/theme.h"
#include "AiUI/view_group.h"
#include "AiUI/widgets/button.h"

#include <vector>

namespace AiUI {

// 按钮条里的一项：按钮实例 + 所属分组。
// 按钮的所有权归基类的子视图列表，这里只记裸指针。
struct ButtonBarItem {
  Button* button = nullptr;
  int group = 0;
};

class ButtonBar : public ViewGroup {
 public:
  ButtonBar() = default;

  // 追加按钮并返回裸指针，调用方挂回调
  Button* addButton(const ButtonSpec& spec, int group = 0);
  Button* buttonAt(int index);
  int buttonCount() const { return static_cast<int>(items_.size()); }
  void clear();

  // 重排全部按钮（内容变化后调用）
  void relayout();

  // 左侧起始留白与按钮间距（96 DPI 设计值）
  void setEdgePad(int pad);
  void setGap(int gap);

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  void arrangeChildren(const Rect& content) override;
  Color backgroundColor() const override { return theme().tool; }

 private:
  std::vector<ButtonBarItem> items_;
  std::vector<Rect> groupLines_;
  int height_ = 32;
  int gap_ = 8;
  int groupGap_ = 10;
  int edgePad_ = 16;
};

}  // namespace AiUI
