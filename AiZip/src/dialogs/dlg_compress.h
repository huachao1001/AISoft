// 压缩对话框
#pragma once

#include "AiUI/platform.h"

#include <string>
#include <vector>

namespace ui {

// 弹出压缩对话框；sources 为要压缩的文件与目录
void ShowCompressDialog(AiUI::NativeHandle owner, const std::vector<std::wstring>& sources);

}  // namespace ui
