// 对话框公共设施：窗口类注册、居中、模态循环、原生控件创建、配色
//
// 对话框内部仍使用系统控件（下拉、复选、进度条），这些属于宿主侧
// 的原生代码，不放进 AiUI 库，避免库的公开头文件暴露 Win32 类型。
#pragma once

#include <windows.h>

#include <functional>

namespace ui {

// 注册窗口类；类已存在时视为成功
bool RegisterWindowClass(const wchar_t* name, WNDPROC proc, HINSTANCE instance, UINT style,
                         HBRUSH background, int iconId = 0);

// 按 owner 居中算出对话框矩形（物理像素）
RECT DialogRect(HWND owner, int width, int height);

// 模态消息循环；退出前恢复 owner
void RunModalDialog(HWND dialog, HWND owner, const std::function<void()>& onBeforeEnable = {});

// 创建子控件并统一设置界面字体；坐标是 96 DPI 设计值
HWND CreateDlgItem(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style, int x,
                   int y, int w, int h, int id, DWORD exStyle = 0);

// 用对话框底色填满客户区
void PaintDialogBackground(HWND hwnd);
// 静态文本与复选框的背景刷；普通按钮返回 nullptr 交给系统
HBRUSH DialogCtlBrush(UINT msg, HWND control);

// 原生控件用的 HFONT（AiUI 的字体是 GDI+ 对象，系统控件不认）
HFONT DialogFont();

}  // namespace ui
