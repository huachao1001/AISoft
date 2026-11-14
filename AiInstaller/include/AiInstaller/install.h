// 安装与卸载：落盘、删除、以及「装完之后通知应用」的标准入口
//
// 本库只做通用的事：把包里的文件放到目标目录、建快捷方式、登记卸载入口、
// 卸载时删干净。应用特有的系统配置（右键菜单、文件关联等）不在这里实现，
// 而是按 config_abi.h 的标准 ABI 调用安装目录里的配置 dll——安装器因此
// 不必知道是哪个应用，更不必认识某个应用私有的导出名。
#pragma once

#include <string>
#include <vector>

#include "AiInstaller/app_info.h"
#include "AiInstaller/package.h"
#include "AiInstaller/path_policy.h"

namespace AiInstaller {

// 安装器运行所需的一切
struct InstallRequest {
  AppInfo app;
  // 目标安装目录，不含尾部分隔符
  std::wstring targetDir;
  Scope scope = Scope::kAllUsers;
};

// 安装进度回调。
//
// fraction 是 0..1 的完成比例，由「文件数 + 配置 + 命令数 + 收尾步骤」计步
// 得出，不是假动画；stage 是给用户看的当前动作，如「正在复制 AiZip.exe」
// 或「正在注册右键菜单」。
using ProgressFn = void (*)(float fraction, const wchar_t* stage, void* user);

// 一次安装的全部输入。
//
// 文件不再是「内嵌资源 id」而是包里的一段字节：同一个安装器 exe 能装任何
// 应用，靠的是运行时读自己末尾的包，而不是构建期编进去的资源。
struct InstallPlan {
  InstallRequest request;
  // 包内文件（含各自在载荷区的位置）
  std::vector<ManifestFile> files;
  // 配置模块文件名（相对安装目录）；空表示该应用没有系统配置
  std::wstring configModule;
  // 安装后按顺序执行的命令。与配置模块并存，两者都算安装步骤。
  std::vector<ManifestCommand> commands;
  ProgressFn progress = nullptr;
  void* progressUser = nullptr;
};

// 执行安装。成功返回空串，失败返回可读原因。
// 失败时已落盘的文件不回滚：留在目录里比删掉更安全，重装会覆盖。
std::wstring runInstall(const InstallPlan& plan);

// 一次卸载的全部输入
struct RemovePlan {
  InstallRequest request;
  // 要删除的文件（相对安装目录）
  std::vector<std::wstring> files;
  // 配置模块文件名；空表示不调用
  std::wstring configModule;
  ProgressFn progress = nullptr;
  void* progressUser = nullptr;
};

// 执行卸载：先撤销配置，再删文件，最后删空目录。
// 自己所在的目录删不掉时，把自身复制到临时目录再让新进程收尾。
std::wstring runRemove(const RemovePlan& plan);

// ── 配置模块（标准 ABI）──
//
// 安装目录里的 <module> 必须按 config_abi.h 导出四个函数。调用方不必知道
// 具体应用：安装器、主程序自愈、卸载走的都是这三个函数。

// 让配置生效。模块缺失/版本不符/调用失败都返回可读原因。
std::wstring applyConfiguration(const std::wstring& installDir,
                                const std::wstring& hostExePath,
                                const std::wstring& moduleName, Scope scope);

// 撤销配置。模块不可用时返回空串：此时删文件不该被一个附加功能拦住。
std::wstring removeConfiguration(const std::wstring& installDir,
                                 const std::wstring& hostExePath,
                                 const std::wstring& moduleName, Scope scope);

// 查询配置是否已就绪且指向 hostExePath。模块不可用时返回 false。
bool isConfigurationReady(const std::wstring& installDir, const std::wstring& hostExePath,
                          const std::wstring& moduleName, Scope scope);

// 执行安装完成后由用户勾选「立即打开」触发的命令。
//
// 与清单里的 [run] afterInstall 对应：装完自动启动会打断用户，所以改成
// 由完成页的复选框决定。命令相对安装目录解析，脱离式启动。
void runAfterInstall(const std::wstring& installDir, const std::wstring& command);

// ── 范围记录 ──

// 读回某次安装记录的范围。记录缺失或损坏时返回 fallback。
//
// 卸载必须用它而不是「当前是否提权」：卸载器的清单带 requireAdministrator，
// 它几乎总是提权状态，靠这个推断会永远清错根键。
Scope readInstalledScope(const std::wstring& installDir, Scope fallback);

// ── 提权 ──

// 是否需要提权才能写进 targetDir（「所有用户」范围且当前未提权）
bool needsElevation(Scope scope);

// 以管理员身份重启当前程序，把 scope 与 targetDir 带过去。
// 返回 true 表示已拉起新进程，调用方应立即退出。
bool relaunchElevated(const InstallRequest& request);

// 提权后的新进程据此还原请求。不完整的命令行返回 false。
bool parseElevatedRequest(const std::vector<std::wstring>& args, InstallRequest& out);

// 把提权参数拼成命令行片段
std::wstring buildElevatedArgs(const InstallRequest& request);

}  // namespace AiInstaller
