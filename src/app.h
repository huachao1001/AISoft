// 应用状态与操作编排
#pragma once

#include "archive.h"
#include "util.h"

#include <windows.h>
#include <string>
#include <vector>

// 列表中的一行
struct Item {
  std::wstring name;
  std::wstring typeName;
  std::wstring method;   // 压缩算法（压缩包模式）
  std::wstring fullPath; // 文件系统模式下的完整路径
  bool isDir = false;
  bool encrypted = false;
  uint64_t size = 0;
  uint64_t packSize = 0;
  uint32_t crc = 0;
  bool hasCrc = false;
  FILETIME mtime{};
  int iconIndex = 0;
  int entryIndex = -1;   // 压缩包模式下的成员下标
};

struct App {
  HINSTANCE instance = nullptr;
  HWND mainWnd = nullptr;
  util::Settings settings;

  // 视图状态
  bool archiveMode = false;
  std::wstring curDir;       // 文件系统模式当前目录
  std::wstring archivePath;  // 压缩包完整路径
  std::wstring innerPath;    // 压缩包内当前目录（空=根）
  arc::Archive archive;
  std::vector<Item> items;

  // 导航历史
  std::vector<std::wstring> history;
  int historyPos = -1;

  int dpi = 96;
  int px(int v) const { return ::MulDiv(v, dpi, 96); }
};

extern App g_app;

// 视图刷新
void AppRefreshView();
void AppNavigateTo(const std::wstring& dir);
void AppOpenArchive(const std::wstring& path);
void AppGoUp();
void AppGoBack();
void AppGoForward();
bool AppCanBack();
bool AppCanForward();

// 外部发起的一次操作（资源管理器右键菜单、单实例转发、命令行）
struct LaunchRequest {
  enum class Action { kCompress, kExtract, kQuickZip };
  Action action = Action::kCompress;
  // 命令行是否显式带了动作开关。没有时为“浏览模式”：开主窗口定位到 paths
  bool explicitAction = false;
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
