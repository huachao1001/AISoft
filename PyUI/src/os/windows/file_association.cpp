// Windows 文件关联：写 ProgID + 取系统图标
//
// 两件事：
//
// 1. **登记**。写 HKCU 下的 Software\Classes：
//      <ext>            → (默认值) = ProgID      「这个后缀由谁打开」
//      <ProgID>         → (默认值) = 描述        「打开方式列表里的名字」
//      <ProgID>\DefaultIcon → "exe,0"            「文件列表里显示的图标」
//      <ProgID>\shell\open\command → "exe "%1""  「双击时的命令行」
//
// 2. **取关联图标**。用 SHGetFileInfo 让 shell 回答「这个文件长什么样」。
//    绝不能自绘一个通用图标：用户要靠图标认出**那个**文件，
//    所有 zip 长得一样等于没有图标。
//
// 关于「不能静默抢占默认程序」：Win8 起 Microsoft 引入了 UserChoice 哈希，
// 程序无法在没有用户交互的情况下成为默认打开方式。这里只能做到「出现在
// 打开方式列表里」，文档里必须说清这一点（见 platform/file_association.h）。
#include "win_platform_hosts.h"

#include "pyui/painter.h"
#include "pyui/surface.h"

#include "win_common.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace pyui {
namespace {

// 注册表键的根：当前用户还是所有用户。
HKEY rootKey(InstallScope scope) {
  return scope == InstallScope::kAllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

// 写一个字符串值（REG_SZ）。成功返回 true。
bool writeString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return false;
  }
  const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  const LONG status = ::RegSetValueExW(key, name, 0, REG_SZ,
                                       reinterpret_cast<const BYTE*>(value.c_str()), bytes);
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

// 读一个字符串值；不存在时返回空串。
std::wstring readString(HKEY root, const std::wstring& path, const wchar_t* name) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return std::wstring();
  }
  wchar_t buffer[1024];
  DWORD size = sizeof(buffer);
  DWORD type = 0;
  const LONG status =
      ::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_SZ) return std::wstring();
  // size 含结尾的 NUL；按 size/2 截断再交给 wstring，
  // 不要假定它一定以 NUL 结尾（有些程序写的时候不带）。
  return std::wstring(buffer, size / sizeof(wchar_t) > 0 ? size / sizeof(wchar_t) - 1 : 0);
}

// 递归删除一个键以及它的所有子键。
// RegDeleteTreeW 在 Vista+ 可用，比手写递归安全（能处理深层的 shell\open\command）。
void deleteTree(HKEY root, const std::wstring& path) {
  ::RegDeleteTreeW(root, path.c_str());
}

// 把 .txt 变成 .txt 的形式（保证带点），并校验不合法后缀。
std::wstring normalizeExtension(const std::string& extension) {
  std::wstring ext = win32::toWidePath(extension);
  if (ext.empty()) return ext;
  if (ext.front() != L'.') ext.insert(ext.begin(), L'.');
  return ext;
}

// 本进程的 exe 路径（注册时留空就用它）。
std::wstring currentExePath() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return std::wstring();
    if (length < buffer.size()) {
      buffer.resize(length);
      return buffer;
    }
    buffer.resize(buffer.size() * 2);
  }
}

// 从 HICON 取一支 32 位 ARGB 位图。实现放在 win_common.cpp：标题栏的
// 应用图标与这里的文件图标要的是同一种东西，两处各写一份必然漂移。

}  // namespace

std::string WindowsFileAssociations::registerFileAssociations(const FileAssociation& association) {
  if (association.progId.empty()) return "progId 不能为空";
  if (association.extensions.empty()) return "至少要指定一个后缀";

  const HKEY root = rootKey(association.scope);
  const std::wstring progId = win32::toWidePath(association.progId);
  const std::wstring classes = L"Software\\Classes\\";

  // 命令行与图标都指向这个 exe；留空取当前进程。
  const std::wstring exe =
      association.hostExePath.empty() ? currentExePath() : win32::toWidePath(association.hostExePath);
  if (exe.empty()) return "无法确定可执行文件路径";

  const std::wstring description = win32::toWide(association.description);
  const std::wstring iconPath =
      association.iconResourceId != 0
          ? exe + L"," + std::to_wstring(association.iconResourceId)
          : exe + L",0";
  // %1 必须加引号：路径里有空格时不加引号，程序只会收到前半段。
  const std::wstring command = L"\"" + exe + L"\" \"%1\"";

  std::string error;
  const std::wstring progKey = classes + progId;
  if (!writeString(root, progKey, nullptr, description.empty() ? progId : description)) {
    error = "无法写入 ProgID：" + win32::lastError("RegSetValueExW");
  }
  if (!writeString(root, progKey + L"\\DefaultIcon", nullptr, iconPath)) {
    if (error.empty()) error = "无法写入 DefaultIcon：" + win32::lastError("RegSetValueExW");
  }
  if (!writeString(root, progKey + L"\\shell\\open\\command", nullptr, command)) {
    if (error.empty()) error = "无法写入 open 命令：" + win32::lastError("RegSetValueExW");
  }

  for (const std::string& extension : association.extensions) {
    const std::wstring ext = normalizeExtension(extension);
    if (ext.size() < 2) {
      if (error.empty()) error = "后缀无效：" + extension;
      continue;
    }
    // 后缀键指向 ProgID。注意这里写的是「默认值」（name == nullptr）。
    if (!writeString(root, classes + ext, nullptr, progId)) {
      if (error.empty()) error = "无法关联 " + extension + "：" + win32::lastError("RegSetValueExW");
    }
    // OpenWithProgids 让「打开方式」列表里出现我们的程序名。
    // 只写 <ext> 的默认值在 Win10 上不够——UserChoice 优先，用户看不到我们。
    HKEY key = nullptr;
    const std::wstring openWith = classes + ext + L"\\OpenWithProgids";
    if (::RegCreateKeyExW(root, openWith.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                          nullptr) == ERROR_SUCCESS) {
      // value 是空 REG_NONE，键名的存在本身就是声明；数据长度按惯例是 0。
      const std::wstring value = progId;
      const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
      ::RegSetValueExW(key, progId.c_str(), 0, REG_NONE, reinterpret_cast<const BYTE*>(value.c_str()),
                       bytes);
      ::RegCloseKey(key);
    }
  }

  // 通知 shell 刷新关联缓存。不发这个，资源管理器会继续用旧图标与旧程序，
  // 用户以为没装上。
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return error;
}

std::string WindowsFileAssociations::unregisterFileAssociations(const FileAssociation& association) {
  if (association.progId.empty()) return "progId 不能为空";
  const HKEY root = rootKey(association.scope);
  const std::wstring progId = win32::toWidePath(association.progId);
  const std::wstring classes = L"Software\\Classes\\";

  for (const std::string& extension : association.extensions) {
    const std::wstring ext = normalizeExtension(extension);
    if (ext.size() < 2) continue;
    const std::wstring extKey = classes + ext;
    // 只在默认值确实指向我们时才清：别人抢占过之后，清掉会让后缀变成
    // 「没有默认程序」，用户所有该类型文件都打不开了。
    const std::wstring current = readString(root, extKey, nullptr);
    if (current == progId) deleteTree(root, extKey);
    deleteTree(root, extKey + L"\\OpenWithProgids");
  }
  deleteTree(root, classes + progId);

  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return std::string();
}

bool WindowsFileAssociations::areFileAssociationsRegistered(const FileAssociation& association) {
  if (association.progId.empty() || association.extensions.empty()) return false;
  const HKEY root = rootKey(association.scope);
  const std::wstring progId = win32::toWidePath(association.progId);
  const std::wstring classes = L"Software\\Classes\\";

  // ProgID 的命令行必须仍指向**当前** exe：安装到别处之后旧登记还在，
  // 但指向的是已经删掉的旧路径，双击会打开失败。
  const std::wstring command =
      readString(root, classes + progId + L"\\shell\\open\\command", nullptr);
  if (command.empty()) return false;
  const std::wstring exe =
      association.hostExePath.empty() ? currentExePath() : win32::toWidePath(association.hostExePath);
  // 命令行是 "\"exe\" \"%1\"" 的形式，比较时只找 exe 子串。
  // 大小写不敏感的比较交给 _wcsicmp 的字符串语义。
  std::wstring lowerCommand = command;
  std::wstring lowerExe = exe;
  std::transform(lowerCommand.begin(), lowerCommand.end(), lowerCommand.begin(),
                 [](wchar_t c) { return ::towlower(c); });
  std::transform(lowerExe.begin(), lowerExe.end(), lowerExe.begin(),
                 [](wchar_t c) { return ::towlower(c); });
  if (lowerCommand.find(lowerExe) == std::wstring::npos) return false;

  for (const std::string& extension : association.extensions) {
    const std::wstring ext = normalizeExtension(extension);
    if (ext.size() < 2) return false;
    if (readString(root, classes + ext, nullptr) != progId) return false;
  }
  return true;
}

InstallScope WindowsFileAssociations::detectScope(const FileAssociation& association) {
  if (association.progId.empty()) return InstallScope::kCurrentUser;
  const std::wstring progId = win32::toWidePath(association.progId);
  const std::wstring key = L"Software\\Classes\\" + progId + L"\\shell\\open\\command";

  // 先问 HKLM：机器级安装是更「强」的那一份，两个都写着时它才生效。
  // 顺序反了会出现「逐项查都对、双击却打开了另一份程序」。
  if (!readString(HKEY_LOCAL_MACHINE, key, nullptr).empty()) return InstallScope::kAllUsers;
  return InstallScope::kCurrentUser;
}

Image WindowsFileAssociations::associationIcon(const AssociationIconRequest& request) {
  const int size = request.pixelSize > 0 ? request.pixelSize : 32;

  // 缓存键包含尺寸：同一个后缀在工具栏里是 16px、在列表里是 32px，
  // 不能共用一个位图（拉伸会糊）。
  std::string key = request.path.empty() ? request.extension : request.path;
  key += '|';
  key += std::to_string(size);
  auto found = icons_.find(key);
  if (found != icons_.end()) return found->second;

  // 有具体文件就按文件问：用户自定义过图标的情况下这才对。
  // 没有文件才退回按后缀问（SHGFI_USEFILEATTRIBUTES 允许对不存在的路径提问）。
  SHFILEINFOW info{};
  DWORD flags = SHGFI_ICON | (size <= 16 ? SHGFI_SMALLICON : SHGFI_LARGEICON);
  std::wstring query;
  if (!request.path.empty()) {
    query = win32::toWidePath(request.path);
  } else if (!request.extension.empty()) {
    query = L"file" + win32::toWidePath(request.extension);
    flags |= SHGFI_USEFILEATTRIBUTES;
  } else {
    return Image();
  }

  Image result;
  if (::SHGetFileInfoW(query.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), flags) != 0 &&
      info.hIcon != nullptr) {
    // SHGFI_LARGEICON 给的是 32px、SHGFI_SMALLICON 是 16px。要更大的尺寸
    // 得用 SHGetImageList，但那要 COM 初始化；32px 缩放到 48/64 足够清楚。
    result = win32::iconToImage(info.hIcon, size <= 16 ? 16 : 32);
    ::DestroyIcon(info.hIcon);
  }

  icons_[key] = result;
  return result;
}

void WindowsFileAssociations::releaseAssociationIcons() {
  // Image 里装的是 Surface*，所有权在这个缓存里。手工 delete 之后
  // 调用方手里的 Image 就悬空了——所以约定是「先 release，再不使用」，
  // 见文档：宿主退出前调用一次。
  for (auto& entry : icons_) {
    if (entry.second.isValid()) {
      delete static_cast<Surface*>(entry.second.handle());
    }
  }
  icons_.clear();
}

}  // namespace pyui
