// 应用状态与操作编排
#pragma once

#include "archive.h"
#include "util.h"

#include <windows.h>
#include <string>
#include <vector>

struct App {
  HINSTANCE instance = nullptr;
  util::Settings settings;

  // 当前打开的压缩包（解压对话框要用它的成员列表）
  arc::Archive archive;
  std::wstring archivePath;

  // 最近一次浏览的目录，仅用于给对话框提供合理的默认路径
  std::wstring curDir;

  int dpi = 96;
  int px(int v) const { return ::MulDiv(v, dpi, 96); }
};

extern App g_app;

// 外部发起的一次操作（资源管理器右键菜单、单实例转发、命令行）
struct LaunchRequest {
  enum class Action { kCompress, kExtract, kQuickZip };
  Action action = Action::kCompress;
  std::vector<std::wstring> paths;
};

// 解析本进程命令行；无动作开关时按后缀推断
LaunchRequest ParseLaunchArgs();

// 单实例转发：动作与路径各占一行
std::wstring SerializeLaunch(const LaunchRequest& req);
LaunchRequest DeserializeLaunch(const std::wstring& packed);

// 执行请求：弹出压缩或解压对话框
void AppDispatchLaunch(const LaunchRequest& req);

// 一键压缩：直接用源文件名产出 <名字>.zip（同名自动改名），不弹参数对话框
void AppQuickZip(const std::vector<std::wstring>& paths);
