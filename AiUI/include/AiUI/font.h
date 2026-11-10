// 字体集：按 DPI 创建一组界面字体，按值传递
#pragma once

#include "AiUI/painter.h"

#include <functional>

namespace AiUI {

class FontSet {
 public:
  Font ui;       // 正文：列表、按钮
  Font uiBold;   // 加粗：标题
  Font uiSmall;  // 小字：状态栏、页签

  void create(int dpi);
  void destroy();
  bool isValid() const { return ui.isValid(); }
};

// 全局字体集，由宿主启动时 create、退出前 destroy
extern FontSet fonts;

// 释放全部字体；宿主退出前调用一次
void releaseFonts();

}  // namespace AiUI
