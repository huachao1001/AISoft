// Tooltip：鼠标停留在控件上时浮出的说明文字
//
// 与 Toast 的区别：Toast 是「刚才那次操作」的回执，弹出即走；Tooltip 依附于
// 某个控件，鼠标一离开就该消失，所以它的生命周期由 ViewRoot 的悬停状态驱动。
//
// 为什么自绘而不接系统 tooltip：系统 tooltip 是另一个原生窗口，配色由系统主题
// 决定。深色主题下会冒出一个白底黑字的框，且各平台外观差异大。自绘在窗口内画，
// 跟着自己的主题走，也不用为三个平台各写一套。
#pragma once

#include "AiUI/painter.h"

#include <string>

namespace AiUI {

class Tooltip {
 public:
  void show(const std::wstring& text);
  void hide();
  bool isVisible() const { return visible_; }
  const std::wstring& text() const { return text_; }

  // 以 anchor（窗口坐标）为基准摆放：优先贴在锚点正上方，
  // 上方放不下就翻到下方，两侧夹在客户区内。
  Rect bounds(const Rect& clientArea, const Rect& anchor, int dpi) const;
  void paint(Painter& painter, const Rect& clientArea, const Rect& anchor, int dpi) const;

 private:
  std::wstring text_;
  bool visible_ = false;
};

}  // namespace AiUI
