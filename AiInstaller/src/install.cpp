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

#include <algorithm>
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

// 安装时新建的目录层级。卸载要把它们收回，否则会留下空壳
// （如 %USERPROFILE%\Programs\AISoft）。只写「确实由安装器创建」的那几级，
// 已存在的目录（Program Files、Users 之类）根本不会进这个文件。
constexpr const wchar_t* kCreatedDirsName = L"installer.dirs";

AiUI::InstallScope toAiScope(Scope scope) {
  return scope == Scope::kAllUsers ? AiUI::InstallScope::kAllUsers
                                   : AiUI::InstallScope::kCurrentUser;
}

// 进度计步器：把整场安装切成固定的若干步，每完成一步推进一格。
//
// 为什么不按字节算比例：复制大文件时如果按字节推进，进度条会长时间停在
// 同一个数字上；按「文件数 + 命令数」计步则每一步都有明确的动作可报，
// 用户看到的文字和进度条同步变化。文件大小差异很大时这一步会慢一点，
// 但不会假。
class Progress {
 public:
  Progress(ProgressFn fn, void* user) : fn_(fn), user_(user) {}

  // 登记总步数。安装开始前一次性算好，中途不再变。
  void reserve(int steps) { total_ = steps > 0 ? steps : 1; }

  // 完成一步：fraction 只增不减，界面据此推进度条
  void step(const wchar_t* stage) {
    if (done_ < total_) ++done_;
    report(stage);
  }

  // 报告一个不回推进度的状态（如开始复制第 n 个文件）
  void report(const wchar_t* stage) const {
    if (!fn_) return;
    const float fraction = static_cast<float>(done_) / static_cast<float>(total_);
    fn_(fraction > 1.0f ? 1.0f : fraction, stage, user_);
  }

 private:
  ProgressFn fn_ = nullptr;
  void* user_ = nullptr;
  int total_ = 1;
  int done_ = 0;
};
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

// 读回记录的目录层级。记录缺失（升级前装的旧版本）时返回空。
std::vector<std::wstring> readCreatedDirs(const std::wstring& installDir) {
  std::vector<std::wstring> dirs;
  if (installDir.empty()) return dirs;

  const std::wstring path = detail::joinPath(installDir, kCreatedDirsName);
  if (!AiUI::platform()->pathExists(path)) return dirs;

  // 目录层级不会多，64 KB 上限足够；读不到就当没有记录
  const std::wstring text = AiUI::platform()->fromUtf8(AiUI::platform()->readFile(path, 64u << 10));
  std::wstring line;
  for (const wchar_t c : text) {
    if (c == L'\n') {
      while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
      if (!line.empty()) dirs.push_back(line);
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
  if (!line.empty()) dirs.push_back(line);
  return dirs;
}

// 记下本次安装新建的目录层级（外层在前），一行一个。
//
// 与已有记录合并而不是覆盖：升级安装时目录都是上一轮建的，这一轮的
// missingDirectories() 会是空表；直接覆盖就把「这些目录由安装器创建」
// 这件事抹掉了，卸载后父目录又会空留着。
void writeCreatedDirs(const std::wstring& installDir, const std::vector<std::wstring>& dirs) {
  std::vector<std::wstring> merged = readCreatedDirs(installDir);
  for (const auto& dir : dirs) {
    if (dir.empty()) continue;
    bool known = false;
    for (const auto& existing : merged) {
      if (AiUI::platform()->equalsPath(existing, dir)) {
        known = true;
        break;
      }
    }
    if (!known) merged.push_back(dir);
  }

  std::wstring text;
  for (const auto& dir : merged) {
    text += dir;
    text += L"\r\n";
  }
  AiUI::platform()->writeFile(detail::joinPath(installDir, kCreatedDirsName),
                              AiUI::platform()->toUtf8(text));
}

// 收尾脚本的路径。
//
// 名字带上进程 id：同一台机器上可能同时卸载两个应用，固定名字会互相覆盖，
// 后启动的那个把前一个的脚本删掉，收尾就断在半路。
std::wstring cleanupScriptPath(const std::wstring& tempDir) {
  return detail::joinPath(tempDir, L"aisetup_uninstall_" +
                                       std::to_wstring(AiUI::platform()->processId()) + L".cmd");
}

// 自我删除：进程删不掉正在运行的自己。写一个脚本，由它在本进程退出后
// 删掉安装目录。
//
// 脚本用 .cmd 而不是单行参数：需要循环、延时和条件跳转，挤在一行里既难读，
// cmd 对 `&` 与标签的处理也容易出错。
void scheduleSelfCleanup(const std::wstring& installDir,
                         const std::vector<std::wstring>& createdDirs) {
  if (installDir.empty()) return;

  const std::wstring tempDir = AiUI::platform()->tempDir();
  if (tempDir.empty()) return;

  const std::wstring scriptPath = cleanupScriptPath(tempDir);
  std::wstring body;
  body += L"@echo off\r\n";
  // 先等本进程退出：安装目录里的卸载程序正被自己占用，早一秒都删不掉
  body += L"ping 127.0.0.1 -n 4 >nul\r\n";
  // 只重试删「本进程占着的那一个文件」，不用 del * 或 rd /s：
  // 目录里可能有用户自己放的东西，按清单删才不会误伤。此时清单登记的
  // 文件、快捷方式、注册表项都已在主流程里处理完，剩下的只有它。
  body += L"set /a tries=0\r\n";
  body += L":retry\r\n";
  body += L"if not exist \"" + installDir + L"\\" + Package::uninstallFileName() +
          L"\" goto done\r\n";
  body += L"del /f /q \"" + installDir + L"\\" + Package::uninstallFileName() +
          L"\" >nul 2>&1\r\n";
  body += L"set /a tries+=1\r\n";
  body += L"if %tries% GEQ 10 goto done\r\n";
  body += L"ping 127.0.0.1 -n 3 >nul\r\n";
  body += L"goto retry\r\n";
  body += L":done\r\n";
  // 目录空了才删得掉；里面还剩用户文件时 rd 会失败，正是想要的结果。
  // 安装目录没了，再收回当年新建的父目录：由内到外，外层目录里还装着
  // 内层目录，顺序反了删不掉。同样只用 rd（不带 /s），绝不连内容一起删。
  body += L"rd \"" + installDir + L"\" >nul 2>&1\r\n";
  for (auto it = createdDirs.rbegin(); it != createdDirs.rend(); ++it) {
    if (it->empty()) continue;
    body += L"rd \"" + *it + L"\" >nul 2>&1\r\n";
  }
  // 脚本用 (goto) 技巧让 cmd 释放文件句柄后再删自身，
  // 否则 Windows 会拒绝删除正在执行的 .cmd。
  body += L"(goto) 2>nul & del /f /q \"%~f0\"\r\n";

  // cmd 按控制台代码页解析 .cmd，默认的 936/437 读不了 UTF-8。路径里可能
  // 有中文用户名（%USERPROFILE%），所以写 UTF-8 并先切到 65001：纯 ASCII
  // 路径在 65001 下逐字节相同，不受影响。
  std::string script = "chcp 65001 >nul\r\n";
  script += AiUI::platform()->toUtf8(body);

  if (!AiUI::platform()->writeFile(scriptPath, script)) {
    // 写不出脚本就没有收尾进程，退回「下次开机删除」这条更慢但可靠的路：
    // 删不掉的那一个是卸载程序自身，不能让它在目录里永远留着。
    AiUI::deleteFileOnReboot(detail::joinPath(installDir, Package::uninstallFileName()));
    return;
  }

  AiUI::launchDetached(L"cmd.exe", L"/c \"" + scriptPath + L"\"", tempDir);
}

// 展开命令里的占位符。
//
// 清单是构建期写死的，但「装到哪个盘」「写 HKLM 还是 HKCU」是运行时才知道的：
// 同一个包在 A 机装 D 盘、在 B 机装 E 盘；选「仅当前用户」时写 HKLM 是越权。
// 让清单写 ${dir} / ${root}，安装器按实际请求展开，包就不必为这些变量做多份。
//
// ${dir}  安装目录，不含尾部分隔符
// ${root} 注册表根键：所有用户 → HKLM，仅当前用户 → HKCU
std::wstring expandPlaceholders(const std::wstring& text, const std::wstring& installDir,
                                Scope scope) {
  if (text.empty()) return text;

  const std::wstring root = scope == Scope::kAllUsers ? L"HKLM" : L"HKCU";
  std::wstring out;
  out.reserve(text.size());

  for (size_t i = 0; i < text.size();) {
    if (text.compare(i, 2, L"${") == 0) {
      const size_t close = text.find(L'}', i + 2);
      if (close != std::wstring::npos) {
        const std::wstring name = text.substr(i + 2, close - i - 2);
        if (name == L"dir") {
          out += installDir;
          i = close + 1;
          continue;
        }
        if (name == L"root") {
          out += root;
          i = close + 1;
          continue;
        }
      }
    }
    out.push_back(text[i]);
    ++i;
  }
  return out;
}

// 执行清单里的一条命令。
//
// path 的解析规则：
//   - 含路径分隔符（如 "C:\x\y.exe" 或 "sub\y.exe"）→ 原样用；相对路径
//     相对安装目录，这样清单里写 "AiZip.exe" 换盘也不用改。
//   - 裸文件名（如 "regsvr32.exe"）→ 交给系统按 PATH 搜索。注册表命令
//     几乎都是系统自带的工具，让清单写全路径反而容易换机器就失效。
//
// 返回空串表示成功；required=false 的命令失败也返回空串（只记日志），
// 因为有些命令本来就是「有则更好」的清理动作。
std::wstring runCommand(const std::wstring& installDir, const ManifestCommand& command,
                        Scope scope) {
  if (command.path.empty()) return {};

  std::wstring exe = expandPlaceholders(command.path, installDir, scope);
  const std::wstring args = expandPlaceholders(command.args, installDir, scope);

  if (exe.find_first_of(L"\\/") != std::wstring::npos &&
      !detail::isAbsolutePath(exe)) {
    exe = detail::joinPath(installDir, exe);
  }

  if (!command.wait) {
    // 不等结果的命令：拉起来就走。安装器退出后它还得活着，所以用脱离式启动。
    AiUI::launchDetached(exe, args, installDir);
    return {};
  }

  const int code = AiUI::runAndWait(exe, args);
  if (code == 0) return {};

  const std::wstring label = command.text.empty() ? command.path : command.text;
  if (!command.required) return {};
  return label + L" 执行失败（退出码 " + std::to_wstring(code) + L"）";
}

// 清单登记的文件可能带子目录（如 "config\x.dll"）。文件删掉之后这些
// 子目录会空着，安装目录就删不掉、新建的父目录也收不回来。
//
// 按层级从深到浅排序：嵌套子目录要先删内层，否则外层永远非空。
std::vector<std::wstring> subdirectoriesOf(const std::vector<std::wstring>& files) {
  std::vector<std::wstring> dirs;
  for (const auto& relative : files) {
    const std::wstring parent = detail::directoryOf(relative);
    if (parent.empty()) continue;
    bool known = false;
    for (const auto& existing : dirs) {
      if (AiUI::platform()->equalsPath(existing, parent)) {
        known = true;
        break;
      }
    }
    if (!known) dirs.push_back(parent);
  }
  std::sort(dirs.begin(), dirs.end(), [](const std::wstring& a, const std::wstring& b) {
    return std::count(a.begin(), a.end(), L'\\') > std::count(b.begin(), b.end(), L'\\');
  });
  return dirs;
}

}  // namespace

void runAfterInstall(const std::wstring& installDir, const std::wstring& command) {
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

  // 不含分隔符时相对安装目录：清单里只写文件名，换盘也不用改
  if (exe.find_first_of(L"\\/") == std::wstring::npos) {
    exe = detail::joinPath(installDir, exe);
  }
  // 脱离式启动：安装器随后就退出了，被启动的程序得自己活着
  AiUI::launchDetached(exe, args, installDir);
}

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

  // 要真正落盘的文件：品牌素材（界面图标）只随包携带，不写到用户目录
  std::vector<ManifestFile> installs;
  for (const auto& file : plan.files) {
    if (file.install && !file.path.empty()) installs.push_back(file);
  }

  // 计步：每个待装文件一步、配置模块一步、每条命令一步，再加记录与收尾几步。
  // 命令与配置都算「安装步骤」，用户看到的进度条因此能反映真实工作量。
  Progress progress(plan.progress, plan.progressUser);
  const int stepFiles = static_cast<int>(installs.size());
  const int stepConfig = plan.configModule.empty() ? 0 : 1;
  const int stepCommands = static_cast<int>(plan.commands.size());
  // 部署卸载程序、写安装记录、快捷方式、卸载入口：各算一步
  progress.reserve(stepFiles + stepConfig + stepCommands + 4);

  // 先记下「哪几级目录现在还不存在」：往后每一步都会创建目录，等装完再
  // 问就分不清哪些是本机原有、哪些是安装器新建的了。卸载靠这份记录决定
  // 能收回哪几级，绝不按目录名去猜系统目录。
  const std::vector<std::wstring> createdDirs =
      detail::missingDirectories(request.targetDir);

  // 1. 落盘：从包里按偏移逐个写出。子目录（如 config\x.dll）自动创建。
  Package package;
  std::wstring packageError;
  if (!package.openSelf(packageError)) {
    return packageError.empty() ? L"安装包不完整：没有找到打包数据" : packageError;
  }

  for (const auto& file : installs) {
    // 进度条下方实时显示正在复制哪个文件：大文件复制时用户不会以为卡死
    const std::wstring stage = L"正在复制 " + file.path;
    progress.report(stage.c_str());
    const std::wstring path = detail::joinPath(request.targetDir, file.path);
    const std::wstring error = package.extractFile(file, path);
    if (!error.empty()) return error;
    progress.step(stage.c_str());
  }

  // 2. 安装器自身：卸载入口指向安装目录里的 uninstall.exe。它必须是
  //    安装器本体的副本——只有它知道怎么按 installer.manifest 反向拆包。
  //    名字固定成 uninstall.exe，用户看到的是「卸载」而不是「安装」。
  const wchar_t* kDeployUninstaller = L"正在部署卸载程序...";
  progress.report(kDeployUninstaller);
  const std::wstring uninstaller =
      detail::joinPath(request.targetDir, Package::uninstallFileName());
  const std::wstring copyError = package.copyInstallerTo(uninstaller);
  if (!copyError.empty()) return copyError;
  progress.step(kDeployUninstaller);

  // 清单也留一份：卸载时从它读回文件表与配置模块名。
  // 装好的 exe 已经被裁掉了载荷，不能再靠它自己回答「当初装了什么」。
  AiUI::platform()->writeFile(
      detail::joinPath(request.targetDir, Package::manifestFileName()),
      AiUI::platform()->toUtf8(package.manifestText()));

  // 3. 应用配置：右键菜单、文件关联等由标准 ABI 的配置模块完成。
  //    安装器不知道这些细节，只知道「装完了叫它一声」。
  if (!plan.configModule.empty()) {
    const wchar_t* kConfig = L"正在注册系统集成...";
    progress.report(kConfig);
    const std::wstring hostExe = detail::joinPath(request.targetDir, request.app.exeName);
    const std::wstring error =
        applyConfiguration(request.targetDir, hostExe, plan.configModule, request.scope);
    if (!error.empty()) return error;
    progress.step(kConfig);
  }

  // 4. 清单里登记的命令：按顺序执行，每条的文案显示在进度条下方。
  //    注册表写入、组件注册这类「一条命令行就够」的事写在这里；
  //    需要 COM 的复杂注册仍走配置模块，两者并存。
  for (const auto& command : plan.commands) {
    if (command.path.empty()) continue;
    const std::wstring stage =
        command.text.empty() ? L"正在执行 " + command.path : command.text;
    progress.report(stage.c_str());
    const std::wstring error = runCommand(request.targetDir, command, request.scope);
    if (!error.empty()) return error;
    progress.step(stage.c_str());
  }

  // 记下范围，卸载时据此清理正确的根键。必须在配置成功之后写：
  // 安装失败时留下的标记会让下次卸载去清一个没写过的东西。
  writeScopeMarker(request.targetDir, request.scope);
  // 目录记录同样要在成功路径上写：失败时安装目录可能根本没用起来
  writeCreatedDirs(request.targetDir, createdDirs);

  // 5. 快捷方式与卸载入口是安装器的通用职责
  const wchar_t* kShortcut = L"正在创建快捷方式...";
  progress.report(kShortcut);
  if (!request.app.exeName.empty()) {
    AiUI::createShortcuts(request.targetDir, request.app.exeName, request.app.displayName, scope);
  }
  progress.step(kShortcut);

  const wchar_t* kUninstallEntry = L"正在登记卸载入口...";
  progress.report(kUninstallEntry);
  const std::wstring uninstallError = AiUI::registerUninstallEntry(
      request.app.displayName, request.app.version, request.app.publisher, request.targetDir,
      uninstaller, L"--uninstall", scope);
  if (!uninstallError.empty()) return uninstallError;
  progress.step(kUninstallEntry);

  // 注意：[run] afterInstall 不在这里执行。装完立刻弹出程序会打断用户，
  // 改成由完成页的「立即打开」复选框决定，见 runAfterInstall()。

  progress.step(L"安装完成");
  return {};
}

std::wstring runRemove(const RemovePlan& plan) {
  const InstallRequest& request = plan.request;

  // 范围以安装时记录的为准。请求里带的 scope 只是「记录缺失时的兜底」，
  // 因为它多半来自「当前是否提权」这类猜测。
  const Scope scopeValue = readInstalledScope(request.targetDir, request.scope);
  const AiUI::InstallScope scope = toAiScope(scopeValue);

  // 卸载也按步推进：撤销配置、删文件、清目录各占一段。
  // 卸载是后台通道，界面通常不显示进度，但签名要保持一致。
  Progress progress(plan.progress, plan.progressUser);
  progress.reserve(3);

  progress.report(L"正在撤销系统集成...");

  // 1. 先撤销应用配置：钩子失败不该阻止删文件，但要记下第一条错误
  std::wstring firstError;
  if (!plan.configModule.empty()) {
    const std::wstring hostExe = detail::joinPath(request.targetDir, request.app.exeName);
    firstError = removeConfiguration(request.targetDir, hostExe, plan.configModule, scopeValue);
  }

  // 2. 撤销安装器自己的登记
  AiUI::removeShortcuts(request.app.displayName, scope);
  AiUI::unregisterUninstallEntry(request.app.displayName, scope);
  progress.step(L"正在撤销系统集成...");

  // 3. 删文件
  progress.report(L"正在删除文件...");
  // 目录记录要先读出来：等会儿它自己也要被删掉
  const std::vector<std::wstring> createdDirs = readCreatedDirs(request.targetDir);
  for (const auto& relative : plan.files) removeFile(request.targetDir, relative);
  // 卸载程序自身与几个内部记录文件都不在清单里，一并删掉。
  // 卸载程序就是当前正在运行的进程，删不掉的部分交给收尾进程。
  removeFile(request.targetDir, Package::uninstallFileName());
  removeFile(request.targetDir, kScopeMarkerName);
  removeFile(request.targetDir, Package::manifestFileName());
  removeFile(request.targetDir, kCreatedDirsName);
  progress.step(L"正在删除文件...");

  // 4. 删空目录。带子目录的文件（如 config\x.dll）删完会留下空目录，
  //    不清掉安装目录就永远非空，父目录也收不回来。
  //    顺序：最内层子目录 → 安装目录 → 安装时新建的父目录（由内到外）。
  progress.report(L"正在清理目录...");
  for (const auto& relative : subdirectoriesOf(plan.files)) {
    removeEmptyDir(detail::joinPath(request.targetDir, relative));
  }
  removeEmptyDir(request.targetDir);
  progress.step(L"正在清理目录...");

  // 5. 自我删除：进程删不掉正在运行的自己，写脚本等本进程退出后收尾。
  //    脚本同时负责删除安装目录与安装时新建的父目录。
  scheduleSelfCleanup(request.targetDir, createdDirs);
  return firstError;
}

}  // namespace AiInstaller
