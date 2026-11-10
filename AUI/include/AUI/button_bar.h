// 按钮条容器：横向排列若干 Button，支持分组分隔线与两端留白
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "AUI/button.h"
#include "AUI/widget.h"

namespace AUI {

// 按钮条里的一项：一个 Button 实例 + 它属于哪个分组
struct ButtonBarItem {
  std::unique_ptr<Button> button;
  int group = 0;  // 分组号变化处画竖分隔线
};

// 横向按钮条：负责创建子按钮、按 DPI 布局、分组分隔线
class ButtonBar : public Widget {
 public:
  // 追加按钮并返回裸指针，调用方可挂回调
  Button* Add(const ButtonSpec& spec, int group = 0);
  Button* At(size_t index);
  void Clear();

  // 内容变化后重排（宽度、可见性、文字变化都调这个）
  void Relayout();
  // 重排时左侧起始留白（96 DPI）
  void SetEdgePad(int pad) {
    edgePad_ = pad;
    Relayout();
  }

 protected:
  void OnSize(int cx, int cy) override;
  void OnPaint() override;
  bool OnEraseBkgnd(HDC dc) override;

 private:
  std::vector<ButtonBarItem> items_;
  std::vector<RECT> groupLines_;
  int height_ = 32;
  int gap_ = 8;
  int groupGap_ = 10;
  int edgePad_ = 16;
  int radius_ = 7;
};

}  // namespace AUI
