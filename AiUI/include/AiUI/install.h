// 安装与卸载的系统能力
//
// 安装程序只做两件事：把文件放到目标目录、写注册表。两者都依赖具体系统
// （自由桌面目录约定、权限模型、注册表位置），所以在这里定义平台无关的
// 接口，src/os/<platform>/ 各自实现。
//
// 应用侧只描述「装什么、装到哪、什么范围」，不出现任何系统 API。
#pragma once

#include "AiUI/shell.h"

#include <string>
#include <vector>

namespace AiUI {

// 安装范围已由 shell.h 的 InstallScope 定义（当前用户 / 所有用户）

// 一个要落地的文件：来源是安装包自身内嵌的资源
struct InstallFile {
  // 内嵌资源 id（RCDATA），由安装程序构建时打进 .rc
  int resourceId = 0;
  // 落地后的文件名，如 "AiZip.exe"
  std::wstring fileName;
};

// 「所有用户」安装需要写到 Program Files 与 HKLM，必须提权。
// 返回 true 表示当前进程已具备该权限。
bool isRunningElevated();

// 以管理员身份重启当前程序（保留命令行）。返回 true 表示已把新进程拉起，
// 调用方应立即退出。用户拒绝提权时返回 false。
// extraArgs 追加在原命令行之后，用于把「提权后该做什么」带给新进程。
bool relaunchElevated(const std::wstring& extraArgs = {});

// 安装目录。scope 为 kCurrentUser 时落在用户目录下，否则落在 Program Files。
// 不含尾部分隔符；目录不必存在。
std::wstring defaultInstallDir(InstallScope scope);

// 程序组显示名，用于默认目录名（如 "AIZip"）
std::wstring applicationDisplayName();

// 动态加载一个模块（Windows 上是 DLL），供「配置能力单独封一个模块」的场景：
// 安装器与主程序都不该把注册表、COM 那套代码各编一份。模块里的导出函数
// 由调用方按自己的约定取用，AiUI 只负责加载与卸载。
//
// 返回的句柄在失败时为空；调用方必须用 unloadModule 释放。
class Module {
 public:
  Module() = default;
  ~Module();

  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;
  Module(Module&& other) noexcept;
  Module& operator=(Module&& other) noexcept;

  // 加载 path 指向的模块；失败返回 false，reason 里是可读原因。
  bool load(const std::wstring& path, std::wstring& reason);

  // 取导出符号；不存在返回 nullptr。调用方自己按函数签名转换。
  void* symbol(const char* name) const;

  bool isLoaded() const { return handle_ != nullptr; }

  void unload();

 private:
  void* handle_ = nullptr;
};

// 在系统「应用与功能」里登记卸载入口。uninstallArgs 是卸载程序的命令行参数，
// 系统据此拉起卸载流程。scope 决定写 HKLM 还是 HKCU。
std::wstring registerUninstallEntry(const std::wstring& displayName,
                                    const std::wstring& displayVersion,
                                    const std::wstring& publisher,
                                    const std::wstring& installDir,
                                    const std::wstring& uninstallCommand,
                                    const std::wstring& uninstallArgs, InstallScope scope);

// 撤销卸载入口
std::wstring unregisterUninstallEntry(const std::wstring& displayName, InstallScope scope);

// 把当前可执行文件复制到 destPath（覆盖同名文件）。卸载程序要删掉自己所在的
// 目录，只能先把自己复制到临时位置再执行真正的删除。
std::wstring copySelfTo(const std::wstring& destPath);

// 启动一个与当前进程无关的进程，不等待它结束。用于「退出后继续收尾」。
// workDir 为空时用该程序所在目录。
bool launchDetached(const std::wstring& exePath, const std::wstring& arguments,
                    const std::wstring& workDir = {});

// 标记一个文件在下次开机时删除。删不掉自己时的最后手段，失败返回 false。
bool deleteFileOnReboot(const std::wstring& path);

// 目录为空时删掉；非空或不存在都返回 false。卸载时清理安装目录用。
bool removeDirectoryIfEmpty(const std::wstring& dir);

// 把内嵌资源写到 targetDir。成功返回空串，失败返回可读原因。
// 目录不存在时自动创建；同名文件会被覆盖。
std::wstring installFiles(const std::wstring& targetDir, const std::vector<InstallFile>& files);

// 把单个内嵌资源写到指定路径（含子目录时自动创建）。成功返回空串。
// 与 installFiles 的区别是不限定目标目录：安装器的清单里允许
// "config\\x.dll" 这类带子目录的相对路径。
std::wstring installSingleResource(int resourceId, const std::wstring& targetPath);

// 从 targetDir 里删除安装的文件（卸载）。返回空串表示成功。
std::wstring removeInstalledFiles(const std::wstring& targetDir,
                                  const std::vector<InstallFile>& files);

// 在资源管理器 / 开始菜单里为已装程序创建快捷方式。
// exeName 是 targetDir 里的可执行文件名，displayName 用作 .lnk 名。
// 空目录或平台不支持时静默跳过，不算失败。
std::wstring createShortcuts(const std::wstring& targetDir, const std::wstring& exeName,
                             const std::wstring& displayName, InstallScope scope);

// 删除快捷方式
std::wstring removeShortcuts(const std::wstring& displayName, InstallScope scope);

// 以指定参数运行某个程序并等待退出，返回其退出码；拉起失败返回 -1。
// 安装程序用它调用已落地的 exe 去完成注册。
int runAndWait(const std::wstring& exePath, const std::wstring& arguments);

}  // namespace AiUI
