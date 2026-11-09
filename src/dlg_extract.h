// 解压对话框
#pragma once

#include <windows.h>

#include <vector>

namespace ui {

// selected 为 g_app.items 的下标；空表示解压整包
void ShowExtractDialog(HWND owner, const std::vector<int>& selected = {});

}  // namespace ui
