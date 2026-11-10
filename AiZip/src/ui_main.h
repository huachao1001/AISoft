// 主窗口：顶部导航 + 工具栏 + 内容分栏 + 状态栏，全部由 XML 布局生成
#pragma once

#include <windows.h>

namespace ui {

bool RegisterMainWindow(HINSTANCE inst);
HWND CreateMainWindow(HINSTANCE inst);
void UpdateMainTitle(HWND hwnd);
// 视图状态变化后同步面包屑与树
void RefreshAddressBar(HWND hwnd);

// 把焦点交回文件列表
void FocusFileList();

// 主窗口级快捷键（Ctrl+L 编辑地址、Ctrl+F 搜索）；返回 true 表示已消费
bool HandleMainShortcut(const MSG& msg);

constexpr int kMinWidth = 860;
constexpr int kMinHeight = 520;

}  // namespace ui
