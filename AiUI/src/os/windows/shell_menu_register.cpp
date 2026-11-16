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

// 释放组件并返回注册表该指向的路径。
//
// 两种命名策略：
//   - 指纹（默认）：资源管理器会一直持有已加载的旧 DLL，同名文件没法替换，
//     所以换名共存，注册表指向新文件；
//   - 固定名：名字可读、目录干净，代价是升级可能撞上「旧文件被持有」。
//     那种情况把新版暂存成 .new 并登记重启替换，注册表继续指向旧文件——
//     旧文件功能完好，没必要为了换个版本让菜单失效一整天。
std::wstring placeComponent(const std::wstring& dir, const std::wstring& fileName,
                            const std::vector<BYTE>& payload, bool fixedName) {
  if (fixedName) {
    const std::wstring path = dir + L"\\" + fileName;
    if (fileMatches(path, payload)) return path;
    if (writeFileAtomic(path, payload.data(), payload.size())) return path;

    // 覆盖失败：多半是资源管理器正持有它。写 .new 排队到重启，
    // 本次安装不失败——新内容已经在机器上，只差一次重启生效。
    const std::wstring staged = path + L".new";
    if (writeFileAtomic(staged, payload.data(), payload.size())) {
      if (!::MoveFileExW(staged.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) {
        ::DeleteFileW(staged.c_str());
      }
    }
    // 旧文件还在就继续用它：内容不一致但不影响这次注册
    return ::PathFileExistsW(path.c_str()) ? path : std::wstring();
  }

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

// 删除一个已不再被引用的文件；删不掉就登记「重启时删除」。
//
// 为什么不能删不掉就放弃：资源管理器加载菜单组件后会把 DLL 一直持有，
// 且不是以 FILE_SHARE_DELETE 打开的——DeleteFile 一律返回「拒绝访问」，
// 下次重装再试仍然失败，残渣就永久留在安装目录里。用户看到的是一个
// 删不掉的安装目录，以及「卸载了但文件还在」。
//
// 微软给这种场景的标准兜底是 MOVEFILE_DELAY_UNTIL_REBOOT：写进注册表的
// PendingFileRenameOperations，重启时由系统在没有任何进程持有它的情况下
// 删除。缺点是要等一次重启，但它不打断用户、不需要强杀 explorer。
//
// 注意：这个 API 要求管理员权限。非提权的卸载登记会失败，此时文件仍然
// 留在磁盘上——但注册表已经不再指向它，功能不受影响，下次重装（通常提权）
// 会再走一遍这里。
void deleteOrDefer(const std::wstring& path) {
  if (::DeleteFileW(path.c_str())) return;
  const DWORD error = ::GetLastError();
  // 文件本来就不在：目的已经达到
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;

  // 被占用（共享冲突 / 拒绝访问）才值得排队；其它错误重试也没意义
  if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) {
    ::MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
  }
}

// 删掉本组件在 dir 里的全部版本（含当前指纹那份）。
//
// 只在注销时用：注册表已经不再指向它们，留着只会让安装目录永远删不掉。
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
    deleteOrDefer((dir + L"\\" + data.cFileName).c_str());
  } while (::FindNextFileW(find, &data));
  ::FindClose(find);

  // 不带指纹的那份（早年版本直接放 aizip.dll）也要清
  deleteOrDefer((dir + L"\\" + componentName).c_str());
}

// 清掉不属于本次注册的组件：旧命名的 aicmd_*.dll 与旧指纹的 aizip_*.dll。
// 资源管理器可能还加载着它们，删不掉就交给 deleteOrDefer 排队到重启。
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
      deleteOrDefer(path);
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

// 「默认应用」登记：只写 ProgID + OpenWithProgids 是不够的，那只是把本程序
// 塞进「打开方式」候选列表，用户得手工去右键里翻。系统「设置 → 默认应用」
// 里要能列出本程序、并支持一键设成默认，还要求两处：
//   - Software\<progId>\Capabilities：声明本程序支持哪些后缀
//   - Software\RegisteredApplications：把本程序登记进系统应用列表
// 缺了它们，系统根本不认为存在这么一个「能打开压缩包的应用程序」，用户在
// 默认应用里连选都选不到。
//
// 命名空间用 Software\<progId>：归自己所有，卸载时整棵删掉不会误伤别人。
bool writeCapabilities(HKEY root, const FileAssociation& association, const std::wstring& exe) {
  const std::wstring base = L"Software\\" + association.progId + L"\\Capabilities";

  if (!regSetString(root, base, L"ApplicationName", association.description)) return false;
  if (!regSetString(root, base, L"ApplicationDescription", association.description)) return false;
  if (!regSetString(root, base, L"ApplicationIcon", exe + L",0")) return false;

  // 支持的后缀逐个映射到自己的 ProgID：系统据此知道本程序能开哪些类型
  for (const auto& ext : association.extensions) {
    if (!regSetString(root, base + L"\\FileAssociations", ext.c_str(), association.progId)) {
      return false;
    }
  }
  return true;
}

// 拆掉「影子登记」：同一个 ProgID 只该有一份。
//
// HKCR 是 HKCU 与 HKLM 的合并视图，HKCU 优先。开发期跑过免安装的副本、
// 或者换过安装位置之后，另一个根键里会留下一份同名 ProgID 指向旧路径，
// 而实际生效的正是它——注册表看着完全正确，双击却打开了另一份程序，
// 甚至是一份已经不存在的 exe。只写自己那一边修不好这种状态，必须把影子
// 拆掉，让系统只剩我们这一份。
//
// ProgID 名是我们独占的命名空间，指向别处的同名登记一定是我们自己残留的，
// 删掉不会误伤别的软件。
void removeShadowRegistration(HKEY otherRoot, const std::wstring& progId, const std::wstring& exe) {
  const std::wstring command = regGetString(
      otherRoot, L"Software\\Classes\\" + progId + L"\\shell\\open\\command", nullptr);
  if (command.empty()) return;
  // 指向同一份程序就是自己另一边的登记，留着（两边都装时需要它）
  if (command.find(exe) != std::wstring::npos) return;

  regDeleteTree(otherRoot, L"Software\\Classes\\" + progId);
  regDeleteTree(otherRoot, L"Software\\Classes\\Applications\\" + progId);
  regDeleteTree(otherRoot, L"Software\\" + progId);
  regDeleteValue(otherRoot, L"Software\\RegisteredApplications", progId.c_str());
}

}  // namespace

std::wstring WindowsBackend::registerShellMenuHandler(const ShellMenuHandler& handler) {
  if (handler.id.empty() || handler.clsid.empty() || handler.componentName.empty()) {
    return L"菜单处理器描述不完整";
  }

  const std::wstring dir = componentDir(handler);
  if (dir.empty()) return L"无法定位组件目录";

  // 组件有两个来源：内嵌在本模块资源里（应用自己带过去），或者已经装在
  // 目录里（安装器把 dll 装好后再调它注册）。后者不重写文件——模块把自己
  // 释放一份副本等于凭空多一个要维护的指纹文件。
  std::wstring dll;
  if (handler.componentResourceId != 0) {
    std::vector<BYTE> payload;
    if (!readEmbedded(handler.componentResourceId, payload)) {
      return L"可执行文件里没有右键菜单组件";
    }
    dll = placeComponent(dir, handler.componentName, payload, handler.fixedComponentName);
    if (dll.empty()) return L"无法释放右键菜单组件";
  } else {
    dll = dir + L"\\" + handler.componentName;
    if (!::PathFileExistsW(dll.c_str())) return L"安装目录里没有右键菜单组件";
  }
  // 旧命名的 aicmd_*.dll / aizip_*.dll 是历史残渣：注册表已经不再指向它们
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

  // 拆掉另一个根键里的影子登记。
  //
  // HKCR 是 HKCU 与 HKLM 的合并视图，HKCU 优先。换过安装范围（或开发期
  // 在构建目录跑过一份）之后，另一边会留着同名 CLSID 指向旧路径：
  // 注册表逐项看都对，右键却加载了另一份 dll，甚至是一份已被删掉的。
  //
  // 只清「指向别处的」那份：它要么指向另一份程序，要么指向一个已经不存在的
  // 文件。两者都该走开——前者是我们自己在别处的旧副本，后者是彻底失效的
  // 残渣。仍然指向本目录组件的另一边登记要留着：所有用户装的那份也服务
  // 别的账户，删了他们的菜单就没了。
  const HKEY other = root == HKEY_CURRENT_USER ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  const std::wstring otherInproc =
      regGetString(other, clsidKey + L"\\InprocServer32", nullptr);
  const bool otherIsOurs =
      ::CompareStringOrdinal(otherInproc.c_str(), -1, dll.c_str(), -1, TRUE) == CSTR_EQUAL;
  if (!otherIsOurs) {
    for (const auto& target : classRootsFor(handler.targets)) {
      regDeleteTree(other, handlerKey(handler, target));
    }
    regDeleteTree(other, clsidKey);
  }

  // 让资源管理器丢掉已缓存的菜单项
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

std::wstring WindowsBackend::unregisterShellMenuHandler(const ShellMenuHandler& handler) {
  // 两个根键都清。
  //
  // 注销时没人能确定"当初装在哪一边"：调用方给的 scope 常常来自
  // 「当前是否提权」这类猜测，而卸载器本身带 requireAdministrator，几乎
  // 总是猜成所有用户。只清一边的后果是另一边的登记活着——用户的右键菜单
  // 明明已经卸载还在，指向一个不存在的 dll。
  //
  // CLSID 与 shellex 挂载都是本程序独占的命名空间（GUID 由应用定义，
  // 处理器 id 带应用前缀），两边同名项一定是自己的，清掉不会误伤别人。
  const HKEY roots[2] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
  for (const HKEY root : roots) {
    if (!handler.id.empty()) {
      for (const auto& target : classRootsFor(handler.targets)) {
        regDeleteTree(root, handlerKey(handler, target));
        // 旧版本用不带 AIUI_ 前缀的键；不清掉会同时出现两套入口
        regDeleteTree(root, L"Software\\Classes\\" + target +
                               L"\\shellex\\ContextMenuHandlers\\" + handler.id);
      }
    }
    if (!handler.clsid.empty()) {
      regDeleteTree(root, L"Software\\Classes\\CLSID\\" + handler.clsid);
    }
  }

  // 组件文件只在「由本模块释放出来」时才清。
  //
  // componentResourceId == 0 表示组件是安装包装好的独立文件，它由安装清单
  // 管理，卸载流程按清单删。这里再删一次有两个坏后果：注销动作往往就是从
  // 那个 dll 里发起的，删自己会留下一份重启删除记录；而「注销再注册」这类
  // 修复动作会因为文件没了而注册失败。
  //
  // 这里不用 componentDir(handler)：它会顺手建目录，而卸载不该创建任何东西。
  const std::wstring dir = handler.componentDir;
  if (handler.componentResourceId != 0 && !dir.empty() && !handler.componentName.empty() &&
      ::PathFileExistsW(dir.c_str())) {
    removeAllComponentVersions(dir, handler.componentName);
  }

  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

bool WindowsBackend::isShellMenuHandlerRegistered(const ShellMenuHandler& handler) {
  if (handler.clsid.empty() || handler.componentName.empty()) return false;

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

  const std::wstring inproc = L"Software\\Classes\\CLSID\\" + handler.clsid + L"\\InprocServer32";
  const std::wstring actualDll = regGetString(root, inproc, nullptr);
  if (actualDll.empty()) return false;

  if (handler.componentResourceId == 0) {
    // 组件就是目录里那一个文件：比对完整路径即可，不看内容。
    // 比内容会让「重启后才替换」的暂存状态被当成未就绪，于是每次启动都
    // 重写一遍注册表——旧文件本来就能用，没有修复的必要。
    const std::wstring expected = dir + L"\\" + handler.componentName;
    if (::CompareStringOrdinal(actualDll.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL) {
      return false;
    }
  } else {
    std::vector<BYTE> payload;
    if (!readEmbedded(handler.componentResourceId, payload)) return false;
    // 文件名带内容指纹时无法按名字比对；只要落在我们的目录且内容就是当前这份即可
    const size_t slash = actualDll.find_last_of(L'\\');
    if (slash == std::wstring::npos || actualDll.substr(0, slash) != dir) return false;
    if (!fileMatches(actualDll, payload)) return false;
  }

  const std::wstring exe = hostExePath(handler.hostExePath);
  if (exe.empty()) return false;
  const std::wstring registeredExe =
      regGetString(root, L"Software\\Classes\\CLSID\\" + handler.clsid + L"\\AppPath", nullptr);
  if (::CompareStringOrdinal(registeredExe.c_str(), -1, exe.c_str(), -1, TRUE) != CSTR_EQUAL) {
    return false;
  }

  // 另一个根键里若有指向别处（或指向已删文件）的同名 CLSID，实际生效的是
  // 它——HKCU 优先于 HKLM。这种状态看着已就绪，右键却是另一份程序在响应，
  // 必须报「未就绪」让上层走一次修复，由注册流程把它拆掉。
  const HKEY other = root == HKEY_CURRENT_USER ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  const std::wstring otherInproc =
      regGetString(other, L"Software\\Classes\\CLSID\\" + handler.clsid + L"\\InprocServer32",
                   nullptr);
  if (!otherInproc.empty() &&
      ::CompareStringOrdinal(otherInproc.c_str(), -1, actualDll.c_str(), -1, TRUE) != CSTR_EQUAL) {
    return false;
  }
  return true;
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
// 系统判为篡改并重置。所以这里分五步，前四步都是「让本程序被系统看见」：
//   1. 建 ProgID（描述 + 图标 + open 命令）；
//   2. 把它加进该后缀的 OpenWithProgids，成为「打开方式」里的候选；
//   3. 把本程序登记到 Applications，让它出现在「选择其他应用」列表；
//   4. 写 Capabilities + RegisteredApplications，让它出现在「默认应用」；
//   5. 没有 UserChoice 的后缀，把默认项直接指向本程序（双击即生效）。
// 已经有 UserChoice 的后缀一律不动——那是用户的决定。
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

  // 4. 默认应用登记：让系统「设置 → 默认应用」里出现本程序，
  //    并把声明的后缀全部映射到本程序的 ProgID。
  if (!writeCapabilities(root, association, exe)) {
    return L"无法登记默认应用能力";
  }
  const std::wstring registeredKey = L"Software\\RegisteredApplications";
  if (!regSetString(root, registeredKey, association.progId.c_str(),
                    L"Software\\" + association.progId + L"\\Capabilities")) {
    return L"无法登记默认应用";
  }

  // 5. 设成默认程序：把每个后缀的默认 ProgID 指向本程序。
  //
  //    系统不允许程序静默写 UserChoice（带用户意图哈希，写了会被判篡改并
  //    重置），但 HKCU\Software\Classes\<ext> 的默认值在没有 UserChoice 时
  //    就是事实上的默认程序，双击直接生效、资源管理器的类型列也跟着变。
  //    用户已经用用户选择框定过别的程序（存在 UserChoice）就一律不动——
  //    那是用户的决定，程序无权覆盖，只保证我们在「打开方式」里可选。
  for (const auto& ext : association.extensions) {
    if (hasUserChoice(ext)) continue;
    regSetString(HKEY_CURRENT_USER, L"Software\\Classes\\" + ext, nullptr, association.progId);
    // 全机范围时 HKLM 那份也一起写：新用户首次登录读的是它
    if (scope == InstallScope::kAllUsers) {
      regSetString(HKEY_LOCAL_MACHINE, L"Software\\Classes\\" + ext, nullptr,
                   association.progId);
    }
  }

  // 6. 拆掉另一边根键里的影子登记。放在最后：这时本边已经写完整，
  //    即使中途失败也不会出现「两边都没了」的空档。
  removeShadowRegistration(root == HKEY_CURRENT_USER ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
                           association.progId, exe);

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
    // 只清掉「指向我们」的默认项；用户后来选了别的程序就别动。
    // 两个根键都查：全机安装时 HKCU 那份也被写过（新用户首次登录读它），
    // 换过安装位置时另一边也可能残留一份指向我们的。
    for (const HKEY probe : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
      const std::wstring current = regGetString(probe, L"Software\\Classes\\" + ext, nullptr);
      if (::CompareStringOrdinal(current.c_str(), -1, association.progId.c_str(), -1, TRUE) ==
          CSTR_EQUAL) {
        regDeleteValue(probe, L"Software\\Classes\\" + ext, nullptr);
      }
    }
  }
  // 两个根键的 ProgID 都拆：换过安装位置时另一边会残留一份，
  // 卸载后它会顶替正确登记，用户看到「卸了还能双击打开压缩包」。
  for (const HKEY probe : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    regDeleteTree(probe, L"Software\\Classes\\" + association.progId);
    regDeleteTree(probe, L"Software\\Classes\\Applications\\" + association.progId);
    // 默认应用登记也要一起撤：留着会让「设置 → 默认应用」里挂着一个指向
    // 已删程序的条目，点了只会报错
    regDeleteValue(probe, L"Software\\RegisteredApplications", association.progId.c_str());
    regDeleteTree(probe, L"Software\\" + association.progId);
  }

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

  // HKCU 会覆盖 HKLM（HKCR 是两者的合并视图，HKCU 优先）。当前用户下留着一份
  // 指向别的路径的同名 ProgID，实际生效的就是它——只检查安装范围那个根键会
  // 漏掉这种情况：注册表看似正确，双击却打开了另一份程序。
  const HKEY other = root == HKEY_CURRENT_USER ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  const std::wstring shadow = regGetString(
      other, L"Software\\Classes\\" + association.progId + L"\\shell\\open\\command", nullptr);
  if (!shadow.empty() && shadow.find(exe) == std::wstring::npos) return false;

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

  // 「默认应用」登记也在校验范围：缺了它系统不会把本程序当成能开压缩包的
  // 应用，「设置 → 默认应用」里看不到，等于关联只做了一半。
  const std::wstring capabilities =
      regGetString(root, L"Software\\RegisteredApplications", association.progId.c_str());
  if (capabilities.empty()) return false;
  // 能力表里至少要有一个后缀，否则系统列表里是个空应用
  if (regGetString(root, L"Software\\" + association.progId + L"\\Capabilities",
                   L"ApplicationName")
          .empty()) {
    return false;
  }
  return true;
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
