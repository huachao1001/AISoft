// 轻量输入框：替代 MFC 的输入弹窗
#pragma once

#include <windows.h>
#include <string>

namespace ui {

// 弹出单行输入框；password 为真时用 ES_PASSWORD
bool PromptText(HWND owner, const std::wstring& title, const std::wstring& label,
                std::wstring& value, bool password = false);

}  // namespace ui
