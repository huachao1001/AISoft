// 右键菜单组件的落地与注册（Windows）
//
// 应用只给一份中性描述（ShellMenuHandler），这里负责：从自身资源取出组件 DLL，
// 写到用户目录，再把 COM 组件与 shellex 挂到 HKCU。
//
// 之所以内嵌而不让用户多拿一个文件：组件必须注册进系统才能被资源管理器加载，
// 但分发时只该给一个 exe。首次运行时释放是唯一两全的做法。
#include "backend.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <string>
#include <vector>

namespace AiUI {
namespace os {
namespace windows {
namespace {

// 组件释放目录。显式给了就用它（安装好的程序用自己所在目录，卸载时一并清掉）；
// 否则按 id 分一层放在用户数据目录，多个应用各占一层。
std::wstring componentDir(const ShellMenuHandler& handler) {
  if (!handler.componentDir.empty()) {
    ::CreateDirectoryW(handler.componentDir.c_str(), nullptr);
    return handler.componentDir;
  }

  const KNOWNFOLDERID& folder = handler.scope == InstallScope::kAllUsers
                                    ? FOLDERID_ProgramData
                                    : FOLDERID_LocalAppData;
  wchar_t* base = nullptr;
  if (FAILED(::SHGetKnownFolderPath(folder, 0, nullptr, &base))) return {};
  std::wstring dir = base;
  ::CoTaskMemFree(base);
  dir += L"\\";
  dir += handler.id;
  ::CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

// 内容指纹：组件文件名带这个后缀，新旧版本不会互相顶掉
unsigned Fingerprint(const std::vector<BYTE>& payload) {
  unsigned hash = 2166136261u;
  for (BYTE b : payload) {
    hash ^= b;
    hash *= 16777619u;
  }
  return hash;
}

bool fileMatches(const std::wstring& path, const std::vector<BYTE>& expected);

// 先写临时文件再改名：写到一半崩溃留下的半个 DLL 会让资源管理器加载失败
bool writeFileAtomic(const std::wstring& path, const void* data, size_t size) {
  const std::wstring temp = path + L".tmp";
  HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;

  const BYTE* p = static_cast<const BYTE*>(data);
  size_t left = size;
  bool ok = true;
  while (left > 0) {
    const DWORD chunk = static_cast<DWORD>(left > (1u << 20) ? (1u << 20) : left);
    DWORD written = 0;
    if (!::WriteFile(file, p, chunk, &written, nullptr) || written == 0) {
      ok = false;
      break;
    }
    p += written;
    left -= written;
  }
  ::CloseHandle(file);

  if (!ok) {
    ::DeleteFileW(temp.c_str());
    return false;
  }
  if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    ::DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

// 释放组件并返回实际落地的路径。
// 文件名带内容指纹：资源管理器会一直持有已加载的旧 DLL，同名文件根本没法替换；
// 换个名字让新旧共存，注册表指向新文件即可。
std::wstring placeComponent(const std::wstring& dir, const std::wstring& fileName,
                            const std::vector<BYTE>& payload) {
  const size_t dot = fileName.find_last_of(L'.');
  const std::wstring stem = fileName.substr(0, dot);
  const std::wstring ext = fileName.substr(dot);

  wchar_t suffix[16] = {};
  ::swprintf_s(suffix, L"_%08X", Fingerprint(payload));
  const std::wstring path = dir + L"\\" + stem + suffix + ext;

  if (fileMatches(path, payload)) return path;
  if (!writeFileAtomic(path, payload.data(), payload.size())) return {};
  return path;
}

bool readEmbedded(int resourceId, std::vector<BYTE>& out) {
  HMODULE self = ::GetModuleHandleW(nullptr);
  HRSRC res = ::FindResourceW(self, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
  if (!res) return false;

  const DWORD size = ::SizeofResource(self, res);
  HGLOBAL loaded = ::LoadResource(self, res);
  if (!loaded || size == 0) return false;

  const void* data = ::LockResource(loaded);
  if (!data) return false;

  out.assign(static_cast<const BYTE*>(data), static_cast<const BYTE*>(data) + size);
  return true;
}

// 大小相同还要比内容：开发期反复构建很容易撞上同样的体积
bool fileMatches(const std::wstring& path, const std::vector<BYTE>& expected) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;

  LARGE_INTEGER size = {};
  const bool sizeOk =
      ::GetFileSizeEx(file, &size) && size.QuadPart == static_cast<LONGLONG>(expected.size());
  if (!sizeOk) {
    ::CloseHandle(file);
    return false;
  }

  std::vector<BYTE> actual(expected.size());
  DWORD read = 0;
  const BOOL ok = ::ReadFile(file, actual.data(), static_cast<DWORD>(actual.size()), &read, nullptr);
  ::CloseHandle(file);
  if (!ok || read != actual.size()) return false;
  return ::memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

// 注册表根键：所有用户写 HKLM（需提权），否则写 HKCU
HKEY classRoot(const ShellMenuHandler& handler) {
  return handler.scope == InstallScope::kAllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

bool regSetString(HKEY root, const std::wstring& subKey, const wchar_t* name,
                  const std::wstring& value) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return false;
  }
  const LONG r = ::RegSetValueExW(key, name, 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(value.c_str()),
                                  static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  ::RegCloseKey(key);
  return r == ERROR_SUCCESS;
}

bool regDeleteTree(HKEY root, const std::wstring& subKey) {
  const LONG r = ::RegDeleteTreeW(root, subKey.c_str());
  return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND;
}

// 读一个字符串值；不存在或类型不对返回空
std::wstring regGetString(HKEY root, const std::wstring& subKey, const wchar_t* name) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return {};

  wchar_t buffer[MAX_PATH * 2] = {};
  DWORD size = sizeof(buffer);
  const LONG r =
      ::RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(buffer), &size);
  ::RegCloseKey(key);
  return r == ERROR_SUCCESS ? std::wstring(buffer) : std::wstring();
}

// 生效对象 → 挂 shellex 的位置
std::vector<std::wstring> classRootsFor(const std::vector<std::wstring>& targets) {
  std::vector<std::wstring> roots;
  if (targets.empty()) {
    roots.push_back(L"*");
    roots.push_back(L"Directory");
    return roots;
  }
  for (const auto& target : targets) {
    if (target == L"*" || target == L"Directory") roots.push_back(target);
    else roots.push_back(L"SystemFileAssociations\\" + target);
  }
  return roots;
}

std::wstring handlerKey(const ShellMenuHandler& handler, const std::wstring& root) {
  return L"Software\\Classes\\" + root + L"\\shellex\\ContextMenuHandlers\\AIUI_" + handler.id;
}

}  // namespace

std::wstring registerShellMenuHandlerImpl(const ShellMenuHandler& handler) {
  if (handler.id.empty() || handler.clsid.empty() || handler.componentName.empty()) {
    return L"菜单处理器描述不完整";
  }

  std::vector<BYTE> payload;
  if (!readEmbedded(handler.componentResourceId, payload)) {
    return L"可执行文件里没有右键菜单组件";
  }

  const std::wstring dir = componentDir(handler);
  if (dir.empty()) return L"无法定位组件目录";
  const std::wstring dll = placeComponent(dir, handler.componentName, payload);
  if (dll.empty()) return L"无法释放右键菜单组件";

  const std::wstring exe = executablePathImpl();
  if (exe.empty()) return L"无法取得程序路径";

  const HKEY root = classRoot(handler);
  const std::wstring clsidKey = L"Software\\Classes\\CLSID\\" + handler.clsid;
  if (!regSetString(root, clsidKey, nullptr, handler.displayName)) return L"无法注册组件标识";

  const std::wstring inproc = clsidKey + L"\\InprocServer32";
  if (!regSetString(root, inproc, nullptr, dll)) return L"无法注册组件路径";
  // 资源管理器在 UI 线程上调用组件，必须是 Apartment
  if (!regSetString(root, inproc, L"ThreadingModel", L"Apartment")) {
    return L"无法注册线程模型";
  }
  // 组件启动谁；写在这里就不必假设组件与 exe 同目录
  if (!regSetString(root, clsidKey + L"\\AppPath", nullptr, exe)) {
    return L"无法登记宿主程序路径";
  }

  for (const auto& target : classRootsFor(handler.targets)) {
    if (!regSetString(root, handlerKey(handler, target), nullptr, handler.clsid)) {
      return L"无法挂载右键菜单：" + target;
    }
    // 旧版本用不带 AIUI_ 前缀的键；不清掉会同时出现两套入口
    regDeleteTree(root,
                  L"Software\\Classes\\" + target + L"\\shellex\\ContextMenuHandlers\\" + handler.id);
  }

  // 让资源管理器丢掉已缓存的菜单项
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

std::wstring unregisterShellMenuHandlerImpl(const ShellMenuHandler& handler) {
  const HKEY root = classRoot(handler);
  for (const auto& target : classRootsFor(handler.targets)) {
    regDeleteTree(root, handlerKey(handler, target));
  }
  if (!handler.clsid.empty()) {
    regDeleteTree(root, L"Software\\Classes\\CLSID\\" + handler.clsid);
  }
  // 组件文件保留：资源管理器可能仍加载着它，删除会失败并留下半残注册
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

bool isShellMenuHandlerRegisteredImpl(const ShellMenuHandler& handler) {
  if (handler.clsid.empty()) return false;

  const HKEY root = classRoot(handler);

  // 组件挂载项必须指向当前 CLSID
  const std::wstring mounted = regGetString(root, handlerKey(handler, L"*"), nullptr);
  if (mounted.empty()) return false;
  if (::CompareStringOrdinal(mounted.c_str(), -1, handler.clsid.c_str(), -1, TRUE) != CSTR_EQUAL) {
    return false;
  }

  // 组件与宿主路径也要对：exe 被挪走或组件升级后必须重写，否则菜单会指向旧位置
  const std::wstring dir = componentDir(handler);
  if (dir.empty()) return false;

  std::vector<BYTE> payload;
  if (!readEmbedded(handler.componentResourceId, payload)) return false;

  const std::wstring inproc = L"Software\\Classes\\CLSID\\" + handler.clsid + L"\\InprocServer32";
  const std::wstring actualDll = regGetString(root, inproc, nullptr);
  if (actualDll.empty()) return false;
  // 文件名带内容指纹，无法按名字比对；只要落在我们的目录且内容就是当前这一份即可
  const size_t slash = actualDll.find_last_of(L'\\');
  if (slash == std::wstring::npos || actualDll.substr(0, slash) != dir) return false;
  if (!fileMatches(actualDll, payload)) return false;

  const std::wstring exe = executablePathImpl();
  if (exe.empty()) return false;
  const std::wstring registeredExe =
      regGetString(root, L"Software\\Classes\\CLSID\\" + handler.clsid + L"\\AppPath", nullptr);
  return ::CompareStringOrdinal(registeredExe.c_str(), -1, exe.c_str(), -1, TRUE) == CSTR_EQUAL;
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
