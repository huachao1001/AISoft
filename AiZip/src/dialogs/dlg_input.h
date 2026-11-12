// 单行输入对话框：替代原来的原生 EDIT 弹窗
#pragma once

#include "AiUI/platform.h"

#include <string>

namespace ui {

// 弹出单行输入框；password 为真时显示为圆点。
// 用户点确定返回 true，并把内容写回 value。
bool PromptText(AiUI::NativeHandle owner, const std::wstring& title, const std::wstring& label,
                std::wstring& value, bool password = false);

}  // namespace ui
