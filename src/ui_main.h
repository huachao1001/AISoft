// 主窗口：工具栏 + 面包屑 + 树 + 列表 + 状态栏
#pragma once

#include <windows.h>

namespace ui {

bool RegisterMainWindow(HINSTANCE inst);
HWND CreateMainWindow(HINSTANCE inst);
void UpdateMainTitle(HWND hwnd);
// 视图状态变化后同步面包屑与状态栏
void RefreshAddressBar(HWND hwnd);
void RefreshStatusBar(HWND hwnd);
// 选中项变化后刷新工具栏按钮可用性
void RefreshToolbarState();
// 主窗口里的文件列表句柄，用于把焦点交回去
HWND MainList();

// 地址栏回车/Esc：由消息循环在 IsDialogMessage 之前调用
bool HandleMainKeyMessage(MSG* msg);
// 编辑框里的按键：消息循环需要跳过 IsDialogMessage，直接派发
bool IsEditKeystroke(const MSG* msg);

constexpr int kToolbarId = 1002;
constexpr int kListId = 1003;
constexpr int kTreeId = 1004;
constexpr int kStatusId = 1005;
constexpr int kPathEditId = 1006;
constexpr int kSearchId = 1007;
constexpr int kCrumbId = 1008;

struct MainParts {
  HWND toolbar = nullptr;
  HWND crumb = nullptr;
  HWND tree = nullptr;
  HWND list = nullptr;
  HWND status = nullptr;
};

}  // namespace ui
