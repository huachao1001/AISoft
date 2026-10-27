// 解压对话框
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace ui {

// selected 为列表行号；空表示全部
void ShowExtractDialog(HWND owner, const std::vector<int>& selected);

}  // namespace ui
