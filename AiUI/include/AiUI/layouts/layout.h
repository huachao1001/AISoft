// 布局基类
//
// 布局是"负责摆放子控件的策略对象"，挂在容器控件上。控件自己仍然负责
// 测量与绘制，布局只决定每个子控件最终的矩形。
#pragma once

#include "AiUI/widget.h"

namespace AiUI {

// 布局计算用的上下文：容器尺寸与内边距
struct LayoutContext {
  Rect content;  // 去掉内边距后的可用区域（物理像素）
  int dpi = 96;
};

class Layout {
 public:
  virtual ~Layout() = default;
  // 摆放子控件；子控件列表由 container.children() 给出
  virtual void arrange(Widget& container, const LayoutContext& context) = 0;
  // 期望尺寸；返回 {0,0} 表示由容器自己决定
  virtual Size measure(Widget& container, const Size& available) { return Size{}; }
};

}  // namespace AiUI
