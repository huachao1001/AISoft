// AUI 自身需要的少量平台查询，避免依赖应用层工具库
#pragma once

#include <windows.h>

#include <string>

namespace AUI {

// 窗口所在显示器的 DPI；取不到时回退 96
int DpiForWindow(HWND hwnd);

// 小写扩展名（不含点）；没有扩展名时返回空串
std::wstring FileExtension(const std::wstring& path);

}  // namespace AUI
