// 工具按钮条：纯自绘，52px 高
#pragma once

#include <windows.h>

namespace ui {

HWND CreateToolBar(HWND parent, HINSTANCE inst, int id);

// 选中状态变化后按钮可用性要重算，下一帧重建布局并重绘
void InvalidateToolBar();

}  // namespace ui
