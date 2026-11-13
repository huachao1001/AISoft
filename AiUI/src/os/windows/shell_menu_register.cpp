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

// 组件释放目录：按 id 分开，多个应用各占一层，卸载时删掉这一层即可
std::wstring componentDir(const ShellMenuHandler& handler) {
  wchar_t* base = nullptr;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) return {};
  std::wstring dir = base;
  ::CoTaskMemFree(base);
  dir += L"\\";
  dir += handler.id;
  ::CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

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
  // 组件正被资源管理器占用时替换会失败，那种情况保留旧文件即可
  if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    ::DeleteFileW(temp.c_str());
    return false;
  }
  return true;
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

bool regSetString(const std::wstring& subKey, const wchar_t* name, const std::wstring& value) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LONG r = ::RegSetValueExW(key, name, 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(value.c_str()),
                                  static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  ::RegCloseKey(key);
  return r == ERROR_SUCCESS;
}

bool regDeleteTree(const std::wstring& subKey) {
  const LONG r = ::RegDeleteTreeW(HKEY_CURRENT_USER, subKey.c_str());
  return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND;
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
  if (dir.empty()) return L"无法定位用户数据目录";
  const std::wstring dll = dir + L"\\" + handler.componentName;
  if (!fileMatches(dll, payload) && !writeFileAtomic(dll, payload.data(), payload.size())) {
    return L"无法释放右键菜单组件：" + dll;
  }

  const std::wstring exe = executablePathImpl();
  if (exe.empty()) return L"无法取得程序路径";

  const std::wstring clsidKey = L"Software\\Classes\\CLSID\\" + handler.clsid;
  if (!regSetString(clsidKey, nullptr, handler.displayName)) return L"无法注册组件标识";

  const std::wstring inproc = clsidKey + L"\\InprocServer32";
  if (!regSetString(inproc, nullptr, dll)) return L"无法注册组件路径";
  // 资源管理器在 UI 线程上调用组件，必须是 Apartment
  if (!regSetString(inproc, L"ThreadingModel", L"Apartment")) return L"无法注册线程模型";
  // 组件启动谁；写在这里就不必假设组件与 exe 同目录
  if (!regSetString(clsidKey + L"\\AppPath", nullptr, exe)) {
    return L"无法登记宿主程序路径";
  }

  for (const auto& root : classRootsFor(handler.targets)) {
    if (!regSetString(handlerKey(handler, root), nullptr, handler.clsid)) {
      return L"无法挂载右键菜单：" + root;
    }
    // 旧版本用不带 AIUI_ 前缀的键；不清掉会同时出现两套入口
    regDeleteTree(L"Software\\Classes\\" + root + L"\\shellex\\ContextMenuHandlers\\" + handler.id);
  }

  // 让资源管理器丢掉已缓存的菜单项
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

std::wstring unregisterShellMenuHandlerImpl(const ShellMenuHandler& handler) {
  for (const auto& root : classRootsFor(handler.targets)) {
    regDeleteTree(handlerKey(handler, root));
  }
  if (!handler.clsid.empty()) {
    regDeleteTree(L"Software\\Classes\\CLSID\\" + handler.clsid);
  }
  // 组件文件保留：资源管理器可能仍加载着它，删除会失败并留下半残注册
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

bool isShellMenuHandlerRegisteredImpl(const ShellMenuHandler& handler) {
  if (handler.clsid.empty()) return false;

  // 组件挂载项必须指向当前 CLSID
  HKEY reg = nullptr;
  const std::wstring key = handlerKey(handler, L"*");
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &reg) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t buffer[64] = {};
  DWORD size = sizeof(buffer);
  const LONG r =
      ::RegQueryValueExW(reg, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(buffer), &size);
  ::RegCloseKey(reg);
  if (r != ERROR_SUCCESS) return false;
  if (::CompareStringOrdinal(buffer, -1, handler.clsid.c_str(), -1, TRUE) != CSTR_EQUAL) {
    return false;
  }

  // 组件与宿主路径也要对：exe 被挪走或组件升级后必须重写，否则菜单会指向旧位置
  const std::wstring dir = componentDir(handler);
  if (dir.empty()) return false;
  const std::wstring expectedDll = dir + L"\\" + handler.componentName;

  std::wstring actualDll;
  const std::wstring inproc = L"Software\\Classes\\CLSID\\" + handler.clsid + L"\\InprocServer32";
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, inproc.c_str(), 0, KEY_READ, &reg) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t pathBuffer[MAX_PATH * 2] = {};
  size = sizeof(pathBuffer);
  const LONG pathResult =
      ::RegQueryValueExW(reg, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(pathBuffer), &size);
  ::RegCloseKey(reg);
  if (pathResult != ERROR_SUCCESS) return false;
  actualDll = pathBuffer;
  if (::CompareStringOrdinal(actualDll.c_str(), -1, expectedDll.c_str(), -1, TRUE) != CSTR_EQUAL) {
    return false;
  }

  const std::wstring exe = executablePathImpl();
  if (exe.empty()) return false;
  const std::wstring appPathKey = L"Software\\Classes\\CLSID\\" + handler.clsid + L"\\AppPath";
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, appPathKey.c_str(), 0, KEY_READ, &reg) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t appBuffer[MAX_PATH * 2] = {};
  size = sizeof(appBuffer);
  const LONG appResult =
      ::RegQueryValueExW(reg, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(appBuffer), &size);
  ::RegCloseKey(reg);
  if (appResult != ERROR_SUCCESS) return false;
  return ::CompareStringOrdinal(appBuffer, -1, exe.c_str(), -1, TRUE) == CSTR_EQUAL;
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
