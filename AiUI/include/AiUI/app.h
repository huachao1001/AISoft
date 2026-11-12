// 应用入口约定
//
// 各平台的图形子系统入口不同（Windows 是 wWinMain，macOS 是 NSApplication，
// Linux 是 main + 事件循环），这些细节由 AiUI 的后端提供。应用只需实现
// runApp()，不必关心自己被谁调用。
#pragma once

namespace AiUI {

// 应用主函数：由应用实现，平台入口调用。
// 返回进程退出码。
int runApp();

}  // namespace AiUI
