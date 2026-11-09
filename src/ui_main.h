// 主窗口：工具栏 + 地址栏 + 文件列表 + 状态栏
#pragma once

#include <windows.h>

namespace ui {

bool RegisterMainWindow(HINSTANCE inst);
HWND CreateMainWindow(HINSTANCE inst);
void UpdateMainTitle(HWND hwnd);
void RefreshAddressBar(HWND hwnd);
void RefreshStatusBar(HWND hwnd);

// 地址栏回车/Esc：由消息循环在 IsDialogMessage 之前调用
bool HandleMainKeyMessage(MSG* msg);

constexpr int kToolbarId = 1001;
constexpr int kListId = 1003;
constexpr int kStatusId = 1005;
constexpr int kPathEditId = 1006;
constexpr int kSearchId = 1007;
constexpr int kBackBtnId = 1008;

struct MainParts {
  HWND toolbar = nullptr;
  HWND list = nullptr;
  HWND status = nullptr;
  HWND pathEdit = nullptr;
  HWND search = nullptr;
  HWND navBar = nullptr;  // 后退/前进/上级/刷新 自绘条
};

}  // namespace ui
