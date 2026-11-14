// 应用状态与操作编排
//
// 不出现平台类型：窗口句柄用 AiUI::NativeHandle，时间用 Unix 纪元毫秒。
#pragma once

#include "archive.h"
#include "util.h"
#include "AiUI/platform.h"

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
  // Unix 纪元毫秒；0 表示未知
  int64_t modifiedMillis = 0;
  int entryIndex = -1;   // 压缩包模式下的成员下标
};

struct App {
  // 主窗口的原生句柄；只用于给系统弹窗做 owner，不参与业务判断
  AiUI::NativeHandle mainWnd = nullptr;
  util::Settings settings;

  // 本窗口接管的单实例 id（按压缩包路径推导）。
  // 窗口销毁时据此释放，否则同一个包关掉后再也打不开。
  std::wstring browseInstanceId;

  // 视图状态
  bool archiveMode = false;
  // 主界面停在「还没打开压缩包」的首页。
  //
  // 与 archiveMode 分开：archiveMode=false 还包含「正在浏览文件系统」，
  // 那种状态下有目录内容可显示，不该显示首页。这个标记只在启动时没有
  // 带路径、以及用户自己关掉/切换出去之后为 true。
  bool homeMode = false;
  std::wstring curDir;       // 文件系统模式当前目录（首页时只作打开对话框的起点）
  std::wstring archivePath;  // 压缩包完整路径
  std::wstring innerPath;    // 压缩包内当前目录（空=根）
  arc::Archive archive;
  std::vector<Item> items;

  // 当前这一层的落地镜像目录：包内条目按包内路径解到这里。
  //
  // 之所以要一个镜像：包内文件没有磁盘路径，而「用系统默认程序打开」和
  // 「把包内压缩包当目录展开」都只能对真实文件操作。落地目录同时充当缓存，
  // 同一个文件双击两次只解一次。目录延迟创建——只是看列表时不该在 temp 里留东西。
  std::wstring stageDir;

  // 嵌套预览：点进包内的压缩包时，当前这一层的状态整体压栈，内层在栈顶工作。
  //
  // 为什么解到临时文件而不是直接开内存流：7-Zip 只接受随机访问输入流，而
  // 内层包在外层包里的数据是压缩的、位置还要现算。解成临时文件是唯一不用
  // 给内核写一套嵌套流实现的办法，代价只是一个临时文件。
  //
  // 为什么连 Archive 一起压栈而不是共用 g_app.archive：解压对话框直接读
  // g_app.archive，共用会让「在内层里解压」变成「解外层的条目」。每层各自
  // 持有一份句柄，语义才和界面看到的一致。
  struct NestedLayer {
    arc::Archive archive;      // 进入内层前那一层
    std::wstring archivePath;  // 那一层的包路径
    std::wstring innerPath;    // 那一层的包内目录
    std::wstring stageDir;     // 那一层自己的镜像目录，退栈时整棵删掉
  };
  std::vector<NestedLayer> nestStack;

  // 导航历史
  std::vector<std::wstring> history;
  int historyPos = -1;

  int dpi = 96;
  int px(int v) const { return (v * dpi + 48) / 96; }
};

extern App g_app;

// 视图刷新
void AppRefreshView();
void AppNavigateTo(const std::wstring& dir);
void AppOpenArchive(const std::wstring& path);

// 点进包内的压缩包：把这个条目解到临时文件后打开，外层状态压栈。
// 返回空串表示成功；非空是可读原因（此时视图不动）。
std::wstring AppEnterNestedArchive(int entryIndex, const std::wstring& entryPath);

// 从嵌套层退回上一层。栈空时返回 false（调用方据此决定是否继续往上退）。
bool AppLeaveNestedArchive();

// 当前是否在嵌套层里（深度 > 0）
bool AppInNestedArchive();

// 关窗/换包时彻底清空嵌套栈与临时文件
void AppClearNestedArchives();

// 用系统默认程序打开包内某个文件。
//
// 包内条目没有磁盘路径，先按包内路径解到当前层的镜像目录（stageDir）再交给
// shell。同一个文件第二次点击时直接复用已解出的副本，不重复解压。
// 返回空串表示成功；非空是可读原因。
std::wstring AppOpenArchiveEntry(int entryIndex, const std::wstring& entryPath);

// 往当前打开的压缩包里的当前目录追加文件/目录。
//
// 7-Zip 内核不支持「往已有容器追加并保持原压缩流」，所以实现是：整包解到
// 临时目录 → 把新文件并进去 → 按原格式重新压回原位。用户看到的是一个进度框，
// 语义上就是「加进这个包」。
// 返回空串表示成功；非空是可读原因。
std::wstring AppAddFilesToArchive(const std::vector<std::wstring>& sources);
void AppGoUp();
void AppGoBack();
void AppGoForward();
bool AppCanBack();
bool AppCanForward();
// 打开指定压缩包并弹出解压对话框（不进入包内浏览）
bool AppExtractArchive(AiUI::NativeHandle owner, const std::wstring& path);
// 解压到压缩包所在目录本身，不建同名子目录
void AppExtractHere(const std::wstring& path);
// 解压到以压缩包主名命名的子目录，目录自动创建
void AppExtractToFolder(const std::wstring& path);

// 外部发起的一次操作（资源管理器右键菜单、单实例转发、命令行）
struct LaunchRequest {
  enum class Action { kCompress, kExtract, kExtractHere, kExtractToFolder, kQuickZip };
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
