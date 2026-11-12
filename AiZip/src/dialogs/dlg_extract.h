// 解压对话框
#pragma once

#include "AiUI/platform.h"

#include <string>
#include <vector>

namespace ui {

// selected 为 g_app.items 的下标；空表示解压整包
// prefix 为包内目录路径：非空时只解压该目录下的成员（左侧树选中目录时用）
void ShowExtractDialog(AiUI::NativeHandle owner, const std::vector<int>& selected = {},
                       const std::wstring& prefix = {});

}  // namespace ui
