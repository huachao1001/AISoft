// 右键菜单组件的落地与注册（Windows）
//
// 应用只给一份中性描述（ShellMenuHandler），这里负责：从自身资源取出组件 DLL，
// 写到用户目录，再把 COM 组件与 shellex 挂到 HKCU。
//
// 之所以内嵌而不让用户多拿一个文件：组件必须注册进系统才能被资源管理器加载，
// 但分发时只该给一个 exe。首次运行时释放是唯一两全的做法。
#include "backend.h"
#include "windows_backend.h"
#include "registry_util.h"

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

// 删掉本组件在 dir 里的全部版本（含当前指纹那份）。
//
// 只在注销时用：注册表已经不再指向它们，留着只会让安装目录永远删不掉。
// 删不掉的（资源管理器仍持有）跳过，留给下次。
//
// 命名规则由 placeComponent 决定（stem_XXXXXXXX.ext），所以清理也放在这里：
// 让调用方去猜「aizip_08D0F419.dll」这种名字，等于把内部约定散播出去。
void removeAllComponentVersions(const std::wstring& dir, const std::wstring& componentName) {
  if (dir.empty() || componentName.empty()) return;

  const size_t dot = componentName.find_last_of(L'.');
  if (dot == std::wstring::npos) return;
  const std::wstring stem = componentName.substr(0, dot);
  const std::wstring ext = componentName.substr(dot);

  const std::wstring pattern = dir + L"\\" + stem + L"_*" + ext;
  WIN32_FIND_DATAW data{};
  HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    ::DeleteFileW((dir + L"\\" + data.cFileName).c_str());
  } while (::FindNextFileW(find, &data));
  ::FindClose(find);

  // 不带指纹的那份（早年版本直接放 aizip.dll）也要清
  ::DeleteFileW((dir + L"\\" + componentName).c_str());
}

// 清掉不属于本次注册的组件：旧命名的 aicmd_*.dll 与旧指纹的 aizip_*.dll。
// 资源管理器可能还加载着它们，删不掉就跳过：注册表已经指向新文件，
// 旧文件留着不影响功能，下次重装还能再试。
void removeLegacyComponents(const std::wstring& dir, const std::wstring& keepPath) {
  const std::wstring patterns[] = {dir + L"\\aicmd_*.dll", dir + L"\\aizip_*.dll"};
  for (const auto& pattern : patterns) {
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) continue;
    do {
      const std::wstring path = dir + L"\\" + data.cFileName;
      if (::CompareStringOrdinal(path.c_str(), -1, keepPath.c_str(), -1, TRUE) == CSTR_EQUAL) {
        continue;
      }
      ::DeleteFileW(path.c_str());
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
  }
}

bool readEmbedded(int resourceId, std::vector<BYTE>& out) {
  // 资源不一定是当前进程的主 exe 内嵌的：组件可以内嵌在别处（例如配置
  // 修复用的 DLL），注册动作又是从那里发起的。所以按「代码所在模块」找，
  // 谁编译进了这段实现，就读谁的内嵌资源。
  HMODULE self = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&readEmbedded), &self)) {
    return false;
  }
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

// 删单个值；值不存在等同于已清理
bool regDeleteValue(HKEY root, const std::wstring& subKey, const wchar_t* name) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_WRITE, &key) != ERROR_SUCCESS) return true;
  const LONG r = ::RegDeleteValueW(key, name);
  ::RegCloseKey(key);
  return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

// 用户是否已为该后缀指定过默认程序。
// Win10 起默认项的事实来源是 Explorer\FileExts\<ext>\UserChoice，它带用户
// 意图哈希，第三方写入会被判为篡改。存在即代表用户做过选择，必须尊重。
bool hasUserChoice(const std::wstring& ext) {
  for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    const std::wstring key =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + ext + L"\\UserChoice";
    HKEY handle = nullptr;
    if (::RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &handle) == ERROR_SUCCESS) {
      ::RegCloseKey(handle);
      return true;
    }
  }
  return false;
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

// 注册该指向哪个宿主程序：显式给了就用它（安装器、修复工具都是替别人注册），
// 只有留空时才是当前进程自己。
std::wstring hostExePath(const std::wstring& explicitPath) {
  return explicitPath.empty() ? os::backend().executablePath() : explicitPath;
}

}  // namespace

std::wstring WindowsBackend::registerShellMenuHandler(const ShellMenuHandler& handler) {
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
  removeLegacyComponents(dir, dll);

  const std::wstring exe = hostExePath(handler.hostExePath);
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

std::wstring WindowsBackend::unregisterShellMenuHandler(const ShellMenuHandler& handler) {
  const HKEY root = classRoot(handler);
  for (const auto& target : classRootsFor(handler.targets)) {
    regDeleteTree(root, handlerKey(handler, target));
  }
  if (!handler.clsid.empty()) {
    regDeleteTree(root, L"Software\\Classes\\CLSID\\" + handler.clsid);
  }

  // 组件文件也要清：安装好的程序把组件放在自己目录里，不删就会出现
  // 「卸载后目录删不掉」的残渣。只是注销菜单（准备立刻重新注册）时，
  // 调用方不该走这里——重新注册会重新释放同一份。
  //
  // 这里不用 componentDir(handler)：它会顺手建目录，而卸载不该创建任何东西。
  const std::wstring dir = handler.componentDir;
  if (!dir.empty() && !handler.componentName.empty() && ::PathFileExistsW(dir.c_str())) {
    removeAllComponentVersions(dir, handler.componentName);
  }

  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

bool WindowsBackend::isShellMenuHandlerRegistered(const ShellMenuHandler& handler) {
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

  const std::wstring exe = hostExePath(handler.hostExePath);
  if (exe.empty()) return false;
  const std::wstring registeredExe =
      regGetString(root, L"Software\\Classes\\CLSID\\" + handler.clsid + L"\\AppPath", nullptr);
  return ::CompareStringOrdinal(registeredExe.c_str(), -1, exe.c_str(), -1, TRUE) == CSTR_EQUAL;
}

InstallScope WindowsBackend::detectShellMenuScope(const ShellMenuHandler& handler) {
  if (handler.clsid.empty()) return InstallScope::kCurrentUser;

  // 只认「已挂载、指向本 CLSID、且 AppPath 就是目标宿主」的根键。
  // 少一个条件都不行：同一台机器可能装着另一份（比如 D 盘的安装版与
  // 开发构建并存），按别人的范围去校准只会让本进程去写自己没权限的根键。
  const std::wstring mountKey = handlerKey(handler, L"*");
  const std::wstring exe = hostExePath(handler.hostExePath);
  if (exe.empty()) return InstallScope::kCurrentUser;

  const std::wstring appPathKey = L"Software\\Classes\\CLSID\\" + handler.clsid + L"\\AppPath";
  for (const HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    const std::wstring mounted = regGetString(root, mountKey, nullptr);
    if (mounted.empty()) continue;
    if (::CompareStringOrdinal(mounted.c_str(), -1, handler.clsid.c_str(), -1, TRUE) != CSTR_EQUAL) {
      continue;
    }
    const std::wstring registeredExe = regGetString(root, appPathKey, nullptr);
    if (::CompareStringOrdinal(registeredExe.c_str(), -1, exe.c_str(), -1, TRUE) != CSTR_EQUAL) {
      continue;
    }
    return root == HKEY_LOCAL_MACHINE ? InstallScope::kAllUsers : InstallScope::kCurrentUser;
  }
  return InstallScope::kCurrentUser;
}

// ── 文件类型关联 ──
//
// Win10 之后系统不允许程序静默抢占默认应用：默认项的最终决定权在
// HKCU\...\FileExts\<ext>\UserChoice，且带有用户意图哈希，第三方写入会被
// 系统判为篡改并重置。能做得正确、也够用的只有三步：
//   1. 建 ProgID（描述 + 图标 + open 命令）；
//   2. 把它加进该后缀的 OpenWithProgids，成为「打开方式」里的候选；
//   3. 把本程序登记到 Applications，让它出现在「选择其他应用」列表。
// 用户在系统弹出的选择框里点一次确认后，默认项就是本程序。
std::wstring WindowsBackend::registerFileAssociations(const FileAssociation& association) {
  if (association.progId.empty() || association.extensions.empty()) {
    return L"文件关联描述不完整";
  }

  const std::wstring exe = hostExePath(association.hostExePath);
  if (exe.empty()) return L"无法取得程序路径";

  const InstallScope scope = currentInstallScope();
  const HKEY root = scope == InstallScope::kAllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;

  // 1. ProgID：显示名、图标、打开命令
  const std::wstring progKey = L"Software\\Classes\\" + association.progId;
  if (!regSetString(root, progKey, nullptr, association.description)) {
    return L"无法登记文件类型标识";
  }
  if (!regSetString(root, progKey + L"\\DefaultIcon", nullptr, exe + L",0")) {
    return L"无法登记文件类型图标";
  }
  // 双击打开走程序默认行为：不带动作开关，由程序按后缀推断
  if (!regSetString(root, progKey + L"\\shell\\open\\command", nullptr,
                    L"\"" + exe + L"\" \"%1\"")) {
    return L"无法登记打开命令";
  }
  if (!regSetString(root, progKey + L"\\shell\\open", L"FriendlyAppName",
                    association.description)) {
    return L"无法登记打开方式名称";
  }

  // 2. 每个后缀：登记候选打开方式
  for (const auto& ext : association.extensions) {
    if (!regSetString(root, L"Software\\Classes\\" + ext + L"\\OpenWithProgids",
                      association.progId.c_str(), L"")) {
      return L"无法登记候选打开方式：" + ext;
    }
  }

  // 3. Applications：出现在「选择其他应用」列表，并声明支持的后缀
  const std::wstring appKey = L"Software\\Classes\\Applications\\" + association.progId;
  if (!regSetString(root, appKey + L"\\shell\\open\\command", nullptr,
                    L"\"" + exe + L"\" \"%1\"")) {
    return L"无法登记应用打开命令";
  }
  if (!regSetString(root, appKey, L"FriendlyAppName", association.description)) {
    return L"无法登记应用显示名";
  }
  for (const auto& ext : association.extensions) {
    if (!regSetString(root, appKey + L"\\SupportedTypes", ext.c_str(), L"")) {
      return L"无法登记应用支持的类型：" + ext;
    }
  }

  // 4. 填补空缺：后缀还没有任何默认程序时，直接把我们设成默认。
  //    用户已经选过别的程序就一律不动——那是用户的决定，程序无权覆盖。
  //    「没有默认」指该后缀既没有 UserChoice，也没有可解析的 ProgID。
  for (const auto& ext : association.extensions) {
    if (hasUserChoice(ext)) continue;
    if (!regGetString(root, L"Software\\Classes\\" + ext, nullptr).empty()) continue;
    regSetString(root, L"Software\\Classes\\" + ext, nullptr, association.progId);
  }

  // 让外壳立刻刷新缓存，否则「打开方式」列表要等资源管理器重启才更新
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

std::wstring WindowsBackend::unregisterFileAssociations(const FileAssociation& association) {
  if (association.progId.empty()) return {};

  const InstallScope scope = currentInstallScope();
  const HKEY root = scope == InstallScope::kAllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;

  for (const auto& ext : association.extensions) {
    regDeleteValue(root, L"Software\\Classes\\" + ext + L"\\OpenWithProgids",
                   association.progId.c_str());
    // 只清掉「指向我们」的默认项；用户后来选了别的程序就别动
    const std::wstring current = regGetString(root, L"Software\\Classes\\" + ext, nullptr);
    if (::CompareStringOrdinal(current.c_str(), -1, association.progId.c_str(), -1, TRUE) ==
        CSTR_EQUAL) {
      regDeleteValue(root, L"Software\\Classes\\" + ext, nullptr);
    }
  }
  regDeleteTree(root, L"Software\\Classes\\" + association.progId);
  regDeleteTree(root, L"Software\\Classes\\Applications\\" + association.progId);

  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

bool WindowsBackend::areFileAssociationsRegistered(const FileAssociation& association) {
  if (association.progId.empty() || association.extensions.empty()) return false;

  const std::wstring exe = hostExePath(association.hostExePath);
  if (exe.empty()) return false;

  const InstallScope scope = currentInstallScope();
  const HKEY root = scope == InstallScope::kAllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;

  const std::wstring registered = regGetString(
      root, L"Software\\Classes\\" + association.progId + L"\\shell\\open\\command", nullptr);
  if (registered.empty()) return false;
  // 程序换位置后旧命令会指向失效路径，必须重写
  if (registered.find(exe) == std::wstring::npos) return false;

  for (const auto& ext : association.extensions) {
    // 候选登记用的是「值名 = ProgID、数据为空串」，所以只能查值名是否存在
    const std::wstring probeKey = L"Software\\Classes\\" + ext + L"\\OpenWithProgids";
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, probeKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    const LONG r = ::RegQueryValueExW(key, association.progId.c_str(), nullptr, nullptr, nullptr,
                                      nullptr);
    ::RegCloseKey(key);
    if (r != ERROR_SUCCESS) return false;
  }
  return true;
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
