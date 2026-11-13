// 单实例聚合：静态动词没有 %*，多选时 shell 会为每个文件各起一个进程（各带 %1）。
// 用命名互斥体分出「收集者」，后到的进程把请求写进收件箱后退出；
// 收集者合并全部路径，只执行一次，避免选十个文件弹十个窗口。
// 互斥体只在收集窗口内持有，收集完就释放——这样对话框开着时再右键仍能独立开一个操作。
#pragma once

#include "app.h"

namespace inst {

// 浏览模式的单实例 id：主窗口持有它，转发也按它找窗口，两处必须一致
constexpr const wchar_t* kBrowserInstanceId = L"AIZip.Browser.v1";

// 收集这一批启动请求。
// 返回 false 表示已转发给收集者、本进程应直接退出；true 表示 merged 可执行。
bool Collect(const LaunchRequest& first, LaunchRequest& merged);

}  // namespace inst
