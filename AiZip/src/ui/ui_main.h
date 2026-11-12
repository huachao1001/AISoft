// 主窗口的宿主接入：拖放与单实例转发
//
// 界面与生命周期在 MainActivity 里。拖放由平台层的 setFileDropHandler 提供，
// 单实例转递走平台层的进程间通道，两者都不需要应用侧碰系统消息。
#pragma once

#include <string>
#include <vector>

namespace ui {

// 主窗口最小尺寸（96 DPI 设计值）
constexpr int kMinWidth = 860;
constexpr int kMinHeight = 520;

// 把拖放与单实例转发挂到主窗口上；MainActivity::onCreate 之后调用
void AttachHostHooks();
// 处理拖进窗口的文件：单个压缩包直接打开，其余弹压缩对话框
void HandleDroppedFiles(const std::vector<std::wstring>& paths);
// 单实例转发：把路径送到已在运行的窗口
bool ForwardPathToRunningWindow(const std::wstring& path);

}  // namespace ui
