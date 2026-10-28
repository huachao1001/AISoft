// 单实例聚合：静态动词没有 %*，多选时 shell 会为每个文件各起一个进程（各带 %1）。
// 用命名互斥体分出「收集者」，后到的进程用 WM_COPYDATA 把路径投递给它后退出；
// 收集者合并全部路径，只弹一次对话框，避免选十个文件弹出十个窗口。
#pragma once

#include "app.h"

namespace inst {

// 收集这一批启动请求。
// 返回 false 表示已转发给收集者、本进程应直接退出；true 表示 merged 可执行。
bool Collect(const LaunchRequest& first, LaunchRequest& merged);

// 取走执行期间新到的请求；没有则 paths 为空
LaunchRequest TakePending();

void Shutdown();

}  // namespace inst
