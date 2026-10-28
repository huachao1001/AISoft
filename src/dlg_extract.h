// 解压对话框
#pragma once

#include <windows.h>

namespace ui {

// 打开解压对话框；压缩包通过 g_app.archive 传入（固定解压整包）
void ShowExtractDialog(HWND owner);

}  // namespace ui
