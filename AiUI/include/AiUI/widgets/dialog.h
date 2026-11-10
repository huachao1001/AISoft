// 宿主接入层：窗口类注册、居中、模态循环
//
// 这个头文件会暴露平台原生类型（Win32 下是 HWND/HBRUSH），供宿主创建
// 顶层窗口与对话框时使用。控件头文件不引用它，控件本身仍然与平台无关。
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace AiUI {

// 注册窗口类；类已存在时视为成功
bool registerWindowClass(const wchar_t* name, WNDPROC proc, HINSTANCE instance, UINT style,
                         HBRUSH background, int iconId = 0);

// 按 owner 居中算出对话框矩形（物理像素）
RECT dialogRect(HWND owner, int width, int height);

// 模态消息循环；退出前恢复 owner
void runModalDialog(HWND dialog, HWND owner,
                    const std::function<void()>& onBeforeEnable = {});

}  // namespace AiUI
