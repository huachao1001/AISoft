// 面包屑导航条：后退 + 路径节点 + 搜索框
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ui {

HWND CreateCrumbBar(HWND parent, HINSTANCE inst, int id);
HWND CrumbSearchEdit();
// 地址栏内容变化后重排并重绘
void RefreshCrumbBar();
// 变成可编辑输入框（点空白处或 Ctrl+L）
void BeginCrumbEdit();
bool CrumbIsEditing();

}  // namespace ui
