// 对话框公共设施：模态循环、居中、控件创建与配色
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace AUI {

// 注册窗口类；图标 id 为 0 时不设图标
bool RegisterWindowClass(const wchar_t* name, WNDPROC proc, HINSTANCE inst, UINT style,
                         HBRUSH bg, int iconId = 0);

// 按父窗口 DPI 缩放坐标创建子控件
HWND CreateDlgItem(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x,
                   int y, int w, int h, int id, DWORD exStyle = 0);

void PaintDialogBackground(HWND hwnd);
// 需要默认按钮绘制时返回 nullptr，交给系统
HBRUSH DialogCtlBrush(UINT msg, HWND ctl);
// 相对 owner 居中的对话框矩形
RECT DialogRect(HWND owner, int w, int h);
// 模态消息循环；退出前恢复 owner
void RunModalDialog(HWND dlg, HWND owner, const std::function<void()>& onBeforeEnable = {});

}  // namespace AUI
