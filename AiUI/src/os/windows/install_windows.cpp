// 安装与卸载（Windows）
//
// 只做三件事：按范围挑目标目录、把内嵌资源落盘、写开始菜单快捷方式。
// 注册表那部分不在这里：右键菜单的注册由已落地的 exe 自己完成，
// 组件是它的内嵌资源，安装器不该再维护一份同样的注册逻辑。
#include "backend.h"

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
  HMODULE self = ::GetModuleHandleW(nullptr);
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
  IShellLinkW* link = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(&link)))) {
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
  return ok;
}

}  // namespace

bool isRunningElevatedImpl() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

  TOKEN_ELEVATION elevation = {};
  DWORD size = sizeof(elevation);
  const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, size, &size);
  ::CloseHandle(token);
  return ok != 0 && elevation.TokenIsElevated != 0;
}

bool relaunchElevatedImpl() {
  const std::wstring exe = executablePathImpl();
  if (exe.empty()) return false;

  // 原命令行原样带过去，安装范围由界面参数决定，不靠提权继承
  std::wstring args;
  for (const auto& arg : commandLineArgsImpl()) {
    if (!args.empty()) args += L' ';
    args += L'"' + arg + L'"';
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

std::wstring defaultInstallDirImpl(InstallScope scope) {
  (void)scope;  // 两种范围共用同一套盘位选择，差别只在子目录的写入权限
  const std::wstring volume = pickInstallVolume();
  if (volume.empty()) return {};
  return volume + L"\\AISoft\\AIZip";
}

std::wstring installFilesImpl(const std::wstring& targetDir, const std::vector<InstallFile>& files) {
  if (targetDir.empty()) return L"安装目录为空";
  if (!ensureDir(targetDir)) return L"无法创建安装目录：" + targetDir;

  for (const auto& file : files) {
    if (file.fileName.empty()) continue;
    const std::wstring path = targetDir + L"\\" + file.fileName;
    if (!writeResource(file.resourceId, path)) {
      return L"无法写出文件：" + path;
    }
  }
  return {};
}

std::wstring removeInstalledFilesImpl(const std::wstring& targetDir,
                                      const std::vector<InstallFile>& files) {  if (targetDir.empty()) return {};
  for (const auto& file : files) {
    if (file.fileName.empty()) continue;
    ::DeleteFileW((targetDir + L"\\" + file.fileName).c_str());
  }
  // 目录为空时一并删掉；非空说明用户放了别的东西，保留
  ::RemoveDirectoryW(targetDir.c_str());
  return {};
}

std::wstring writeInstallMarkerImpl(const std::wstring& targetDir, const std::wstring& marker,
                                    const std::string& content) {
  if (targetDir.empty() || marker.empty()) return L"安装目录或标记名为空";
  if (!ensureDir(targetDir)) return L"无法创建安装目录：" + targetDir;

  const std::wstring path = targetDir + L"\\" + marker;
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return L"无法写入安装标记：" + path;

  DWORD written = 0;
  if (!content.empty()) {
    ::WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
  }
  ::CloseHandle(file);
  return {};
}

std::wstring createShortcutsImpl(const std::wstring& targetDir, const std::wstring& exeName,
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

std::wstring removeShortcutsImpl(const std::wstring& displayName, InstallScope scope) {
  const std::wstring programs = startMenuPrograms(scope);
  if (programs.empty()) return {};
  ::DeleteFileW((programs + L"\\" + displayName + L".lnk").c_str());
  return {};
}

int runAndWaitImpl(const std::wstring& exePath, const std::wstring& arguments) {
  if (exePath.empty()) return -1;

  std::wstring command = L'"' + exePath + L'"';
  if (!arguments.empty()) {
    command += L' ';
    command += arguments;
  }

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  if (!::CreateProcessW(exePath.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
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

}  // namespace windows
}  // namespace os
}  // namespace AiUI
