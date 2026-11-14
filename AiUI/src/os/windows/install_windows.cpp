// 安装与卸载（Windows）
//
// 只做三件事：按范围挑目标目录、把内嵌资源落盘、写开始菜单快捷方式。
// 注册表那部分不在这里：右键菜单的注册由已落地的 exe 自己完成，
// 组件是它的内嵌资源，安装器不该再维护一份同样的注册逻辑。
#include "backend.h"
#include "windows_backend.h"
#include "registry_util.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <string>
#include <vector>

namespace AiUI {
namespace os {
namespace windows {
namespace {

// 取已知目录；不存在时返回空
std::wstring knownFolder(const KNOWNFOLDERID& id) {
  wchar_t* base = nullptr;
  if (FAILED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &base))) return {};
  std::wstring dir = base;
  ::CoTaskMemFree(base);
  return dir;
}

// 递归创建目录，逐级建，最后一级失败才算失败
bool ensureDir(const std::wstring& dir) {
  if (dir.empty()) return false;
  if (::PathFileExistsW(dir.c_str())) return true;

  std::wstring parent = dir;
  const size_t slash = parent.find_last_of(L'\\');
  if (slash != std::wstring::npos && slash > 2) {
    if (!ensureDir(parent.substr(0, slash))) return false;
  }
  return ::CreateDirectoryW(dir.c_str(), nullptr) != 0 || ::GetLastError() == ERROR_ALREADY_EXISTS;
}

// 从自身资源取出一个 RCDATA 并整块写出
bool writeResource(int resourceId, const std::wstring& path) {
  // 按「代码所在模块」找资源：安装器与配置修复 DLL 都可能调用安装文件落地，
  // 各自的资源在自己的映像里，不能一律当成当前进程的主 exe。
  HMODULE self = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&writeResource), &self)) {
    return false;
  }
  HRSRC res = ::FindResourceW(self, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
  if (!res) return false;

  const DWORD size = ::SizeofResource(self, res);
  HGLOBAL loaded = ::LoadResource(self, res);
  if (!loaded || size == 0) return false;
  const void* data = ::LockResource(loaded);
  if (!data) return false;

  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;

  DWORD written = 0;
  const BOOL ok = ::WriteFile(file, data, size, &written, nullptr);
  ::CloseHandle(file);
  return ok != 0 && written == size;
}

// 快捷方式路径：所有用户放公共开始菜单，否则放当前用户
std::wstring startMenuPrograms(InstallScope scope) {
  return scope == InstallScope::kAllUsers ? knownFolder(FOLDERID_CommonPrograms)
                                          : knownFolder(FOLDERID_Programs);
}

// 用 IShellLink 写一个 .lnk；工作目录设成程序目录，免得相对路径解析到别处
bool createShellLink(const std::wstring& linkPath, const std::wstring& target,
                     const std::wstring& workDir, const std::wstring& description) {
  // IShellLink 是 COM 对象：本模块可能是被一个尚未初始化 COM 的 dll 调用的，
  // 不能假定调用方已经建好套间。这里自己配对初始化，失败（含
  // RPC_E_CHANGED_MODE：线程已有套间）就沿用现有套间继续。
  const HRESULT comInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool ownCom = SUCCEEDED(comInit);

  IShellLinkW* link = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(&link)))) {
    if (ownCom) ::CoUninitialize();
    return false;
  }

  link->SetPath(target.c_str());
  link->SetWorkingDirectory(workDir.c_str());
  link->SetDescription(description.c_str());

  bool ok = false;
  IPersistFile* file = nullptr;
  if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
    ok = SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
    file->Release();
  }
  link->Release();
  if (ownCom) ::CoUninitialize();
  return ok;
}

}  // namespace

bool WindowsBackend::isRunningElevated() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

  TOKEN_ELEVATION elevation = {};
  DWORD size = sizeof(elevation);
  const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, size, &size);
  ::CloseHandle(token);
  return ok != 0 && elevation.TokenIsElevated != 0;
}

bool WindowsBackend::relaunchElevated(const std::wstring& extraArgs) {
  const std::wstring exe = executablePath();
  if (exe.empty()) return false;

  // 原命令行原样带过去，安装范围由界面参数决定，不靠提权继承。
  // 只给含空格的参数加引号：无脑全加会把 --dir="C:\x" 变成 "--dir="C:\x""。
  auto quoteIfNeeded = [](const std::wstring& arg) {
    if (arg.find(L' ') == std::wstring::npos) return arg;
    return L'"' + arg + L'"';
  };
  std::wstring args;
  for (const auto& arg : commandLineArgs()) {
    if (!args.empty()) args += L' ';
    args += quoteIfNeeded(arg);
  }
  if (!extraArgs.empty()) {
    if (!args.empty()) args += L' ';
    args += extraArgs;
  }

  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = exe.c_str();
  info.lpParameters = args.empty() ? nullptr : args.c_str();
  info.nShow = SW_SHOWNORMAL;
  if (!::ShellExecuteExW(&info)) return false;
  if (info.hProcess) ::CloseHandle(info.hProcess);
  return true;
}

// 选一个可写的数据盘：优先非系统盘，从 D 起按字母序找，
// 只有系统盘可用时才落到 C。可移动盘与光驱跳过。
std::wstring pickInstallVolume() {
  wchar_t win[MAX_PATH] = {};
  const wchar_t systemDrive =
      (::GetWindowsDirectoryW(win, MAX_PATH) != 0) ? win[0] : L'C';

  auto usable = [](wchar_t letter) {
    if (letter < L'A' || letter > L'Z') return false;
    const std::wstring root = std::wstring(1, letter) + L":\\";
    // 只认固定盘：可移动盘拔掉后程序就没了
    if (::GetDriveTypeW(root.c_str()) != DRIVE_FIXED) return false;
    return ::GetDiskFreeSpaceExW(root.c_str(), nullptr, nullptr, nullptr) != 0;
  };

  // D 到 Z：跳过系统盘，命中即用
  for (wchar_t c = L'D'; c <= L'Z'; ++c) {
    if (c == systemDrive) continue;
    if (usable(c)) return std::wstring(1, c) + L":";
  }
  // 退而求其次：系统盘
  if (usable(systemDrive)) return std::wstring(1, systemDrive) + L":";
  return {};
}

std::wstring WindowsBackend::defaultInstallDir(InstallScope scope) {
  (void)scope;  // 两种范围共用同一套盘位选择，差别只在子目录的写入权限
  const std::wstring volume = pickInstallVolume();
  if (volume.empty()) return {};
  return volume + L"\\AISoft\\AIZip";
}

std::wstring WindowsBackend::installFiles(const std::wstring& targetDir, const std::vector<InstallFile>& files) {
  if (targetDir.empty()) return L"安装目录为空";
  if (!ensureDir(targetDir)) return L"无法创建安装目录：" + targetDir;

  for (const auto& file : files) {
    if (file.fileName.empty()) continue;
    const std::wstring path = targetDir + L"\\" + file.fileName;
    const std::wstring error = installSingleResource(file.resourceId, path);
    if (!error.empty()) return error;
  }
  return {};
}

std::wstring WindowsBackend::removeInstalledFiles(const std::wstring& targetDir,
                                                  const std::vector<InstallFile>& files) {
  if (targetDir.empty()) return {};
  for (const auto& file : files) {
    if (file.fileName.empty()) continue;
    ::DeleteFileW((targetDir + L"\\" + file.fileName).c_str());
  }
  // 目录为空时一并删掉；非空说明用户放了别的东西，保留
  ::RemoveDirectoryW(targetDir.c_str());
  return {};
}

std::wstring WindowsBackend::createShortcuts(const std::wstring& targetDir,
                                             const std::wstring& exeName,
                                             const std::wstring& displayName, InstallScope scope) {
  if (targetDir.empty() || exeName.empty()) return {};

  const std::wstring exe = targetDir + L"\\" + exeName;
  if (!::PathFileExistsW(exe.c_str())) return {};

  const std::wstring programs = startMenuPrograms(scope);
  if (programs.empty()) return {};
  if (!ensureDir(programs)) return L"无法创建开始菜单目录";

  const std::wstring link = programs + L"\\" + displayName + L".lnk";
  return createShellLink(link, exe, targetDir, displayName) ? std::wstring()
                                                            : L"无法创建快捷方式";
}

std::wstring WindowsBackend::removeShortcuts(const std::wstring& displayName, InstallScope scope) {
  const std::wstring programs = startMenuPrograms(scope);
  if (programs.empty()) return {};
  ::DeleteFileW((programs + L"\\" + displayName + L".lnk").c_str());
  return {};
}

int WindowsBackend::runAndWait(const std::wstring& exePath, const std::wstring& arguments) {
  if (exePath.empty()) return -1;

  // CreateProcessW 的 lpApplicationName 不查 PATH，传裸名字（如 "reg.exe"）
  // 会直接失败。清单里写系统工具是合理的期待，这里补一次解析——与
  // launchDetached 同一套规则。
  std::wstring appPath = exePath;
  if (appPath.find_first_of(L"\\/") == std::wstring::npos) {
    wchar_t resolved[MAX_PATH] = {};
    const DWORD len = ::SearchPathW(nullptr, appPath.c_str(), L".exe", MAX_PATH, resolved, nullptr);
    if (len == 0 || len >= MAX_PATH) return -1;
    appPath = resolved;
  }

  std::wstring command = L'"' + appPath + L'"';
  if (!arguments.empty()) {
    command += L' ';
    command += arguments;
  }

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  if (!::CreateProcessW(appPath.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
    return -1;
  }

  ::WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(pi.hProcess, &code);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}

void* WindowsBackend::loadModule(const std::wstring& path) {
  if (path.empty()) return nullptr;
  // LOAD_WITH_ALTERED_SEARCH_PATH：模块可能在安装目录里，依赖同目录的其它
  // 文件。默认按系统目录优先搜索会加载到别处的同名组件。
  return ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

void* WindowsBackend::moduleSymbol(void* module, const char* name) {
  if (!module || !name) return nullptr;
  return reinterpret_cast<void*>(
      ::GetProcAddress(static_cast<HMODULE>(module), name));
}

void WindowsBackend::unloadModule(void* module) {
  if (module) ::FreeLibrary(static_cast<HMODULE>(module));
}

std::wstring WindowsBackend::registerUninstallEntry(const std::wstring& displayName,
                                                    const std::wstring& displayVersion,
                                                    const std::wstring& publisher,
                                                    const std::wstring& installDir,
                                                    const std::wstring& uninstallCommand,
                                                    const std::wstring& uninstallArgs,
                                                    InstallScope scope) {
  if (displayName.empty() || uninstallCommand.empty()) return L"卸载项描述不完整";

  const HKEY root = scope == InstallScope::kAllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  // 卸载项放在 Uninstall 下；键名用显示名，保证同一程序只有一个入口
  const std::wstring key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + displayName;

  // 命令行拆成「可执行文件」与「参数」两个值，系统在「应用与功能」里
  // 直接用它们拉起卸载程序。忽略的键会让条目变成无法点击的僵尸项，所以全部要写。
  if (!regSetString(root, key, L"DisplayName", displayName)) return L"无法登记卸载项名称";
  if (!displayVersion.empty() && !regSetString(root, key, L"DisplayVersion", displayVersion)) {
    return L"无法登记卸载项版本";
  }
  if (!publisher.empty() && !regSetString(root, key, L"Publisher", publisher)) {
    return L"无法登记卸载项发布者";
  }
  if (!installDir.empty() && !regSetString(root, key, L"InstallLocation", installDir)) {
    return L"无法登记卸载项安装位置";
  }
  if (!regSetString(root, key, L"UninstallString",
                    L"\"" + uninstallCommand + L"\" " + uninstallArgs)) {
    return L"无法登记卸载命令";
  }
  // 有些系统工具只读 QuietUninstallString；没有静默卸载时留空不写
  if (!regSetString(root, key, L"DisplayIcon", uninstallCommand + L",0")) {
    return L"无法登记卸载项图标";
  }
  return {};
}

std::wstring WindowsBackend::unregisterUninstallEntry(const std::wstring& displayName,
                                                      InstallScope scope) {
  if (displayName.empty()) return {};
  const HKEY root = scope == InstallScope::kAllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  regDeleteTree(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + displayName);
  return {};
}

std::wstring WindowsBackend::copySelfTo(const std::wstring& destPath) {
  if (destPath.empty()) return L"目标路径为空";
  const std::wstring self = executablePath();
  if (self.empty()) return L"无法取得自身路径";
  if (::CopyFileW(self.c_str(), destPath.c_str(), FALSE) == 0) {
    return L"无法复制到：" + destPath;
  }
  return {};
}

bool WindowsBackend::launchDetached(const std::wstring& exePath, const std::wstring& arguments,
                                    const std::wstring& workDir) {
  if (exePath.empty()) return false;

  // CreateProcessW 的 lpApplicationName 必须是完整路径：它不查 PATH，
  // 传裸名字（如 "cmd.exe"）会直接失败。调用方写裸名字是合理的期待，
  // 所以在这里补一次解析。
  std::wstring appPath = exePath;
  if (appPath.find_first_of(L"\\/") == std::wstring::npos) {
    wchar_t resolved[MAX_PATH] = {};
    const DWORD len = ::SearchPathW(nullptr, appPath.c_str(), L".exe", MAX_PATH, resolved, nullptr);
    if (len == 0 || len >= MAX_PATH) return false;
    appPath = resolved;
  }

  std::wstring command = L'"' + appPath + L'"';
  if (!arguments.empty()) {
    command += L' ';
    command += arguments;
  }

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');

  // 不继承句柄：卸载收尾进程要能活过调用方退出。
  //
  // 用 CREATE_NO_WINDOW 而不是 DETACHED_PROCESS：后者会让子进程完全没有
  // 控制台，cmd.exe 拿不到控制台会直接退出、一条命令都不执行——收尾脚本
  // 就永远不会跑。CREATE_NO_WINDOW 只是不给窗口，标准句柄仍可用。
  const DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;

  const wchar_t* dir = workDir.empty() ? nullptr : workDir.c_str();
  if (!::CreateProcessW(appPath.c_str(), buffer.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        dir, &si, &pi)) {
    return false;
  }
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
}

bool WindowsBackend::deleteFileOnReboot(const std::wstring& path) {
  if (path.empty()) return false;
  // MOVEFILE_DELAY_UNTIL_REBOOT 要求管理员权限；失败时上层会放弃清理，
  // 留下的文件不影响功能，比假装成功更好。
  return ::MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != 0;
}

std::wstring WindowsBackend::installSingleResource(int resourceId,
                                                   const std::wstring& targetPath) {
  if (targetPath.empty()) return L"目标路径为空";

  // 允许清单里写带子目录的路径（如 config\x.dll），先把父目录建出来
  const size_t slash = targetPath.find_last_of(L'\\');
  if (slash != std::wstring::npos && slash > 2) {
    const std::wstring parent = targetPath.substr(0, slash);
    if (!ensureDir(parent)) return L"无法创建目录：" + parent;
  }

  if (!writeResource(resourceId, targetPath)) {
    return L"无法写出文件：" + targetPath;
  }
  return {};
}

bool WindowsBackend::removeDirectoryIfEmpty(const std::wstring& dir) {
  if (dir.empty()) return false;
  // RemoveDirectory 只在目录为空时成功，正好是我们要的语义
  return ::RemoveDirectoryW(dir.c_str()) != 0;
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
