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
bool relaunchElevated();

// 安装目录。scope 为 kCurrentUser 时落在用户目录下，否则落在 Program Files。
// 不含尾部分隔符；目录不必存在。
std::wstring defaultInstallDir(InstallScope scope);

// 程序组显示名，用于默认目录名（如 "AIZip"）
std::wstring applicationDisplayName();

// 把内嵌资源写到 targetDir。成功返回空串，失败返回可读原因。
// 目录不存在时自动创建；同名文件会被覆盖。
std::wstring installFiles(const std::wstring& targetDir, const std::vector<InstallFile>& files);

// 从 targetDir 里删除安装的文件（卸载）。返回空串表示成功。
std::wstring removeInstalledFiles(const std::wstring& targetDir,
                                  const std::vector<InstallFile>& files);

// 写下安装标记。程序用它区分「安装版」与开发期构建：安装版把右键组件
// 放在自己目录下，卸载时能一并清掉。content 里的范围关键字（all-users）
// 也决定程序启动时往哪个根键注册。
std::wstring writeInstallMarker(const std::wstring& targetDir, const std::wstring& marker,
                                const std::string& content);

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
