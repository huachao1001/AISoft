// ButtonBar：横向按钮条，支持分组与分组间的竖分隔线
#pragma once

#include "AiUI/theme.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widget.h"

#include <memory>
#include <vector>

namespace AiUI {

// 按钮条里的一项：按钮实例 + 所属分组。
// 按钮的所有权归基类的子控件列表，这里只记裸指针。
struct ButtonBarItem {
  Button* button = nullptr;
  int group = 0;
};

class ButtonBar : public Widget {
 public:
  ButtonBar() { className_ = L"AiUIButtonBar"; }

  // 追加按钮并返回裸指针，调用方挂回调
  Button* add(const ButtonSpec& spec, int group = 0);
  Button* at(size_t index);
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
  Color backgroundColor() const override { return kColorTool; }

 private:
  std::vector<ButtonBarItem> items_;
  std::vector<Rect> groupLines_;
  int height_ = 32;
  int gap_ = 8;
  int groupGap_ = 10;
  int edgePad_ = 16;
};

}  // namespace AiUI
