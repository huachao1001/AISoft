// 应用侧配色：对话框里原生控件的背景色需要单独一份
#pragma once

#include <windows.h>

namespace ui {

// 对话框底色：分量单独给出，供 GDI 刷子与 GDI+ 绘制共用
constexpr BYTE kDialogR = 240;
constexpr BYTE kDialogG = 247;
constexpr BYTE kDialogB = 252;
constexpr COLORREF kDialogBg = RGB(kDialogR, kDialogG, kDialogB);

}  // namespace ui
