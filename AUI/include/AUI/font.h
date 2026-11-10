// 字体集：按 DPI 创建一组界面字体
#pragma once

#include <windows.h>

namespace AUI {

class FontSet {
 public:
  HFONT ui = nullptr;       // 10pt 常规：列表正文、按钮
  HFONT uiBold = nullptr;   // 10pt 半粗
  HFONT uiSmall = nullptr;  // 9pt 常规：状态栏、页签

  void Create(int dpi);
  void Destroy();
};

extern FontSet fonts;

}  // namespace AUI
