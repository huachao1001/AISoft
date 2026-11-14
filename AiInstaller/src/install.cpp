// 安装与卸载的通用流程
//
// 系统动作全部经 AiUI 的中性接口：落盘、快捷方式、卸载入口、提权、脱离式
// 启动都在后端实现。本文件只编排顺序，配置交给标准 ABI 的配置模块。
#include "AiInstaller/install.h"

#include "config_module.h"
#include "internal_util.h"

#include "AiInstaller/package.h"
#include "AiUI/install.h"
#include "AiUI/platform.h"

#include <string>
#include <vector>

namespace AiInstaller {
namespace {

// 安装范围记录文件。卸载时必须知道当初装在哪个范围，否则会去清另一个根键：
// 装成 HKCU 却按 HKLM 清，用户看到的是「卸载了但菜单还在」。
//
// 不能靠「当前进程是否提权」推断——卸载器的清单里带 requireAdministrator，
// 它几乎总是提权状态，那样永远会猜成「所有用户」。
constexpr const wchar_t* kScopeMarkerName = L"installer.scope";

AiUI::InstallScope toAiScope(Scope scope) {
  return scope == Scope::kAllUsers ? AiUI::InstallScope::kAllUsers
                                   : AiUI::InstallScope::kCurrentUser;
}

void report(const ProgressFn& fn, void* user, const wchar_t* stage) {
  if (fn) fn(stage, user);
}

// 从安装目录里删掉一个文件；不存在视为成功
void removeFile(const std::wstring& installDir, const std::wstring& relative) {
  if (relative.empty()) return;
  const std::wstring path = detail::joinPath(installDir, relative);
  if (AiUI::platform()->pathExists(path)) AiUI::platform()->deleteFile(path);
}

// 目录为空时删掉。非空说明用户放了别的东西，保留。
void removeEmptyDir(const std::wstring& dir) {
  if (dir.empty()) return;
  const auto entries = AiUI::platform()->listDirectory(dir, false);
  if (!entries.empty()) return;
  AiUI::platform()->removeDirectoryIfEmpty(dir);
}

// 记下本次安装用的范围，供卸载时读回。
void writeScopeMarker(const std::wstring& installDir, Scope scope) {
  const std::string value = scope == Scope::kAllUsers ? "all-users" : "current-user";
  AiUI::platform()->writeFile(detail::joinPath(installDir, kScopeMarkerName), value);
}

// 收尾进程用的临时目录与脚本名。
//
// 名字带上进程 id：同一台机器上可能同时装两个应用，固定名字会互相覆盖，
// 后启动的那个把前一个的副本删掉，卸载就断在半路。
std::wstring cleanupHelperPath(const std::wstring& tempDir) {
  return detail::joinPath(tempDir, L"aisetup_uninstall_" +
                                       std::to_wstring(AiUI::platform()->processId()) + L".exe");
}

// 自我删除：进程删不掉正在运行的自己。把自身复制到临时目录，
// 由新进程等本进程退出后删除安装目录。
//
// 收尾逻辑写成 .cmd 文件而不是单行参数：需要循环、延时和条件跳转，
// 挤在一行里既难读，cmd 对 `&` 与标签的处理也容易出错。
void scheduleSelfCleanup(const std::wstring& installDir) {
  if (installDir.empty()) return;

  const std::wstring self = AiUI::platform()->executablePath();
  if (self.empty()) return;

  const std::wstring tempDir = AiUI::platform()->tempDir();
  if (tempDir.empty()) return;

  const std::wstring helper = cleanupHelperPath(tempDir);
  if (!AiUI::copySelfTo(helper).empty()) return;

  const std::wstring scriptPath =
      detail::joinPath(tempDir, L"aisetup_uninstall_" +
                                    std::to_wstring(AiUI::platform()->processId()) + L".cmd");
  std::wstring body;
  body += L"@echo off\r\n";
  // 先等本进程退出：安装目录里的安装器正被自己占用，早一秒都删不掉
  body += L"ping 127.0.0.1 -n 4 >nul\r\n";
  // 逐个删文件再删目录：rd /s /q 会因为个别文件被占用而整体失败。
  // 资源管理器释放右键组件往往要几秒，所以重试到目录消失为止。
  body += L"set /a tries=0\r\n";
  body += L":retry\r\n";
  body += L"del /f /q \"" + installDir + L"\\*\" >nul 2>&1\r\n";
  body += L"rd /s /q \"" + installDir + L"\" >nul 2>&1\r\n";
  body += L"if not exist \"" + installDir + L"\" goto done\r\n";
  body += L"set /a tries+=1\r\n";
  body += L"if %tries% GEQ 10 goto done\r\n";
  body += L"ping 127.0.0.1 -n 3 >nul\r\n";
  body += L"goto retry\r\n";
  body += L":done\r\n";
  // 收尾：复制出来的副本和脚本自己都要删。脚本用 (goto) 技巧让 cmd
  // 释放文件句柄后再删自身，否则 Windows 会拒绝删除正在执行的 .cmd。
  body += L"del /f /q \"" + helper + L"\" >nul 2>&1\r\n";
  body += L"(goto) 2>nul & del /f /q \"%~f0\"\r\n";

  // writeFile 收的是窄字符串：cmd 不认 UTF-8 无 BOM，这里的路径是 ASCII，
  // 脚本内容也刻意不含中文，逐字符收窄是安全的。
  std::string narrow;
  narrow.reserve(body.size());
  for (const wchar_t c : body) narrow.push_back(static_cast<char>(c));

  if (!AiUI::platform()->writeFile(scriptPath, narrow)) {
    // 写不出脚本就没有收尾进程，退回「下次开机删除」这条更慢但可靠的路
    AiUI::deleteFileOnReboot(self);
    AiUI::deleteFileOnReboot(helper);
    return;
  }

  AiUI::launchDetached(L"cmd.exe", L"/c \"" + scriptPath + L"\"", tempDir);
}

// 安装完成后执行清单里的命令。
//
// 命令形如 "AiZip.exe" 或 "AiZip.exe --open"：第一段是程序，其余是参数。
// 不含路径分隔符时相对安装目录解析，这样清单里只写文件名，换盘也不用改。
void runAfterInstallCommand(const std::wstring& installDir, const std::wstring& command) {
  if (command.empty() || installDir.empty()) return;

  std::wstring exe = command;
  std::wstring args;
  const size_t space = command.find(L' ');
  if (space != std::wstring::npos) {
    exe = command.substr(0, space);
    args = command.substr(space + 1);
  }
  // 去掉可能存在的包裹引号
  if (exe.size() >= 2 && exe.front() == L'"' && exe.back() == L'"') {
    exe = exe.substr(1, exe.size() - 2);
  }
  if (exe.empty()) return;

  if (exe.find_first_of(L"\\/") == std::wstring::npos) {
    exe = detail::joinPath(installDir, exe);
  }
  AiUI::launchDetached(exe, args, installDir);
}

}  // namespace

Scope readInstalledScope(const std::wstring& installDir, Scope fallback) {
  if (installDir.empty()) return fallback;
  const std::wstring path = detail::joinPath(installDir, kScopeMarkerName);
  if (!AiUI::platform()->pathExists(path)) return fallback;

  // 值不认识时退回 fallback：最保守的选择是不会因为猜错而失败的那边。
  const std::string value = AiUI::platform()->readFile(path, 64);
  if (value == "all-users") return Scope::kAllUsers;
  if (value == "current-user") return Scope::kCurrentUser;
  return fallback;
}

bool needsElevation(Scope scope) {
  return scope == Scope::kAllUsers && !AiUI::isRunningElevated();
}

std::wstring buildElevatedArgs(const InstallRequest& request) {
  std::wstring args = L"--elevated-retry";
  args += request.scope == Scope::kAllUsers ? L" --scope=all-users" : L" --scope=current-user";
  args += L" --dir=\"" + request.targetDir + L"\"";
  return args;
}

bool parseElevatedRequest(const std::vector<std::wstring>& args, InstallRequest& out) {
  bool retry = false;
  std::wstring dir;
  Scope scope = Scope::kAllUsers;

  for (const auto& arg : args) {
    if (arg == L"--elevated-retry") {
      retry = true;
    } else if (arg == L"--scope=all-users") {
      scope = Scope::kAllUsers;
    } else if (arg == L"--scope=current-user") {
      scope = Scope::kCurrentUser;
    } else if (arg.rfind(L"--dir=", 0) == 0) {
      dir = arg.substr(6);
      if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') {
        dir = dir.substr(1, dir.size() - 2);
      }
    }
  }

  if (!retry || dir.empty()) return false;
  out.targetDir = dir;
  out.scope = scope;
  return true;
}

bool relaunchElevated(const InstallRequest& request) {
  return AiUI::relaunchElevated(buildElevatedArgs(request));
}

std::wstring applyConfiguration(const std::wstring& installDir,
                                const std::wstring& hostExePath,
                                const std::wstring& moduleName, Scope scope) {
  return detail::callConfigModule(installDir, hostExePath, moduleName, scope, true).error;
}

std::wstring removeConfiguration(const std::wstring& installDir,
                                 const std::wstring& hostExePath,
                                 const std::wstring& moduleName, Scope scope) {
  return detail::callConfigModule(installDir, hostExePath, moduleName, scope, false).error;
}

bool isConfigurationReady(const std::wstring& installDir, const std::wstring& hostExePath,
                          const std::wstring& moduleName, Scope scope) {
  return detail::queryConfigModule(installDir, hostExePath, moduleName, scope);
}

std::wstring runInstall(const InstallPlan& plan) {
  const InstallRequest& request = plan.request;
  const AiUI::InstallScope scope = toAiScope(request.scope);

  if (request.targetDir.empty()) return L"安装目录为空";

  report(plan.progress, plan.progressUser, L"正在写入文件...");

  // 1. 落盘：从包里按偏移逐个写出。子目录（如 config\x.dll）自动创建。
  Package package;
  std::wstring packageError;
  if (!package.openSelf(packageError)) {
    return packageError.empty() ? L"安装包不完整：没有找到打包数据" : packageError;
  }

  for (const auto& file : plan.files) {
    if (file.path.empty()) continue;
    // 品牌素材（界面图标）只随包携带，不落到用户目录
    if (!file.install) continue;
    const std::wstring path = detail::joinPath(request.targetDir, file.path);
    const std::wstring error = package.extractFile(file, path);
    if (!error.empty()) return error;
  }

  // 2. 安装器自身：卸载入口指向安装目录里的它，用户从「应用与功能」
  //    拉起时才能正确推算出安装目录。少了这一步，卸载会删错地方。
  report(plan.progress, plan.progressUser, L"正在部署卸载程序...");
  const std::wstring selfInstaller =
      detail::joinPath(request.targetDir, package.installerFileName());
  const std::wstring copyError = package.copyInstallerTo(selfInstaller);
  if (!copyError.empty()) return copyError;

  // 清单也留一份：卸载时从它读回文件表与配置模块名。
  // 装好的 exe 已经被裁掉了载荷，不能再靠它自己回答「当初装了什么」。
  AiUI::platform()->writeFile(
      detail::joinPath(request.targetDir, Package::manifestFileName()),
      AiUI::platform()->toUtf8(package.manifestText()));

  // 3. 应用配置：右键菜单、文件关联等由标准 ABI 的配置模块完成。
  //    安装器不知道这些细节，只知道「装完了叫它一声」。
  if (!plan.configModule.empty()) {
    report(plan.progress, plan.progressUser, L"正在注册系统集成...");
    const std::wstring hostExe = detail::joinPath(request.targetDir, request.app.exeName);
    const std::wstring error =
        applyConfiguration(request.targetDir, hostExe, plan.configModule, request.scope);
    if (!error.empty()) return error;
  }

  // 记下范围，卸载时据此清理正确的根键。必须在配置成功之后写：
  // 安装失败时留下的标记会让下次卸载去清一个没写过的东西。
  writeScopeMarker(request.targetDir, request.scope);

  // 4. 快捷方式与卸载入口是安装器的通用职责
  report(plan.progress, plan.progressUser, L"正在创建快捷方式...");
  if (!request.app.exeName.empty()) {
    AiUI::createShortcuts(request.targetDir, request.app.exeName, request.app.displayName, scope);
  }

  report(plan.progress, plan.progressUser, L"正在登记卸载入口...");
  const std::wstring uninstallError = AiUI::registerUninstallEntry(
      request.app.displayName, request.app.version, request.app.publisher, request.targetDir,
      selfInstaller, L"--uninstall", scope);
  if (!uninstallError.empty()) return uninstallError;

  // 5. 安装后命令：清单里写了才执行，失败不影响安装结果
  if (!request.app.afterInstall.empty()) {
    runAfterInstallCommand(request.targetDir, request.app.afterInstall);
  }

  report(plan.progress, plan.progressUser, L"安装完成");
  return {};
}

std::wstring runRemove(const RemovePlan& plan) {
  const InstallRequest& request = plan.request;

  // 范围以安装时记录的为准。请求里带的 scope 只是「记录缺失时的兜底」，
  // 因为它多半来自「当前是否提权」这类猜测。
  const Scope scopeValue = readInstalledScope(request.targetDir, request.scope);
  const AiUI::InstallScope scope = toAiScope(scopeValue);

  report(plan.progress, plan.progressUser, L"正在撤销系统集成...");

  // 1. 先撤销应用配置：钩子失败不该阻止删文件，但要记下第一条错误
  std::wstring firstError;
  if (!plan.configModule.empty()) {
    const std::wstring hostExe = detail::joinPath(request.targetDir, request.app.exeName);
    firstError = removeConfiguration(request.targetDir, hostExe, plan.configModule, scopeValue);
  }

  // 2. 撤销安装器自己的登记
  AiUI::removeShortcuts(request.app.displayName, scope);
  AiUI::unregisterUninstallEntry(request.app.displayName, scope);

  // 3. 删文件
  report(plan.progress, plan.progressUser, L"正在删除文件...");
  for (const auto& relative : plan.files) removeFile(request.targetDir, relative);
  // 范围标记与安装清单都是清单之外的内部文件，一并删掉
  removeFile(request.targetDir, kScopeMarkerName);
  removeFile(request.targetDir, Package::manifestFileName());

  // 4. 删空目录。安装器自己还在里面，删不掉的部分交给收尾进程。
  report(plan.progress, plan.progressUser, L"正在清理目录...");
  removeEmptyDir(request.targetDir);

  // 5. 自我删除：把自己复制到临时目录，由新进程删除原安装目录。
  scheduleSelfCleanup(request.targetDir);
  return firstError;
}

}  // namespace AiInstaller
