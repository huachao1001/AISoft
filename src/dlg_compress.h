// 压缩对话框
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace ui {

// 打开压缩对话框；sources 为空时压缩当前目录
void ShowCompressDialog(HWND owner, const std::vector<std::wstring>& sources);

}  // namespace ui
