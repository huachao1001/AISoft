// 主窗口的原生宿主接入
//
// 界面本体在 MainActivity（main_activity.h）：XML 布局、控件装配、生命周期。
// 这里只剩绕不开的平台细节：把拖放与单实例转发挂到 AiUI 建的窗口上。
#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace ui {

// 主窗口最小尺寸（96 DPI 设计值）
constexpr int kMinWidth = 860;
constexpr int kMinHeight = 520;

// 把拖放与单实例转发挂到主窗口上；MainActivity::onCreate 之后调用
void AttachHostHooks(HWND host);
// 处理拖进窗口的文件：单个压缩包直接打开，其余弹压缩对话框
void HandleDroppedFiles(HWND host, HDROP drop);
// 单实例转发：把路径送到已在运行的窗口
bool ForwardPathToRunningWindow(const std::wstring& path);

}  // namespace ui
