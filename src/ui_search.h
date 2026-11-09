// 圆角搜索框：自绘边框与提示文字，避免原生 EDIT 的方角与系统主题
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace ui {

HWND CreateSearchBox(HWND parent, HINSTANCE inst, int id, const std::wstring& cue);
std::wstring SearchBoxText(HWND box);
void SetSearchBoxCue(HWND box, const std::wstring& cue);
// 内嵌 EDIT 句柄，供消息循环判断焦点
HWND SearchBoxEdit(HWND box);
// 内容变化回调
void SetSearchBoxChangeHandler(HWND box, const std::function<void()>& fn);

}  // namespace ui
