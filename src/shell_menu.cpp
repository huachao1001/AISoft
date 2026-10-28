// Windows 资源管理器右键菜单注册：HKCU\Software\Classes，无需管理员权限
#include "shell_menu.h"

#include "util.h"

#include <shlobj.h>

#include <vector>

namespace shell {
namespace {

// 动词名带上产品前缀，避免和别的压缩软件撞车
constexpr const wchar_t* kVerbCompress = L"AIZip.Compress";
constexpr const wchar_t* kVerbExtract = L"AIZip.Extract";

// 右键菜单里显示的文字
constexpr const wchar_t* kLabelCompress = L"压缩该文件...";
constexpr const wchar_t* kLabelExtract = L"解压该文件...";

// 命令行开关：资源管理器通过它告诉程序该做什么
constexpr const wchar_t* kArgCompress = L"-compress";
constexpr const wchar_t* kArgExtract = L"-extract";

std::wstring ExePath() {
  wchar_t buf[MAX_PATH * 2]{};
  const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  return std::wstring(buf, n);
}

// 支持的压缩包后缀，和 Archive::IsArchiveFile 保持一致
const std::vector<std::wstring>& ArchiveExts() {
  static const std::vector<std::wstring> kExts = {
      L".zip", L".7z",  L".rar", L".tar",  L".gz",  L".tgz", L".bz2",
      L".tbz2", L".xz", L".txz", L".cab",  L".iso", L".wim", L".jar",
      L".apk", L".epub", L".lzh", L".arj", L".z",   L".001"};
  return kExts;
}

// 写一个默认值（REG_SZ）。已存在同名值时覆盖。
bool SetString(const std::wstring& subKey, const wchar_t* name, const std::wstring& value) {
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

bool ReadString(const std::wstring& subKey, const wchar_t* name, std::wstring& out) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t buf[1024]{};
  DWORD size = sizeof(buf);
  DWORD type = 0;
  const LONG r = ::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
  ::RegCloseKey(key);
  if (r != ERROR_SUCCESS || type != REG_SZ) return false;
  out.assign(buf, size / sizeof(wchar_t));
  while (!out.empty() && out.back() == L'\0') out.pop_back();
  return true;
}

// 递归删除整棵子树；键不存在也算成功
bool DeleteTree(const std::wstring& subKey) {
  LONG r = ::RegDeleteTreeW(HKEY_CURRENT_USER, subKey.c_str());
  if (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND) return true;
  return r == ERROR_SUCCESS;
}

// 一个动词挂在某个 ProgID 下：返回写入的第一个失败点，空串表示成功
std::wstring WriteVerb(const std::wstring& parent, const wchar_t* verb, const wchar_t* label,
                       const std::wstring& exe, const wchar_t* argSwitch) {
  const std::wstring base = parent + L"\\shell\\" + verb;
  const std::wstring cmdKey = base + L"\\command";

  if (!SetString(base, nullptr, label)) return L"写入菜单文字失败：" + base;
  if (!SetString(base, L"Icon", exe)) return L"写入图标失败：" + base;

  // MultiSelectModel=Player + %*：多选时只启动一个进程，全部路径一起传进来，
  // 避免选十个文件弹出十个压缩对话框
  if (!SetString(base, L"MultiSelectModel", L"Player")) return L"写入多选模式失败：" + base;

  const std::wstring cmd = L"\"" + exe + L"\" " + argSwitch + L" %*";
  if (!SetString(cmdKey, nullptr, cmd)) return L"写入命令行失败：" + cmdKey;
  return L"";
}

}  // namespace

std::wstring RegisterContextMenu() {
  const std::wstring exe = ExePath();
  if (exe.empty()) return L"无法取得程序路径";

  // 1) 所有文件（包括未知类型）都挂上压缩项
  std::wstring err = WriteVerb(L"Software\\Classes\\*", kVerbCompress, kLabelCompress, exe,
                               kArgCompress);
  if (!err.empty()) return err;
  // 2) 文件夹同样可以压缩
  err = WriteVerb(L"Software\\Classes\\Directory", kVerbCompress, kLabelCompress, exe, kArgCompress);
  if (!err.empty()) return err;

  // 3) 压缩包后缀挂解压项。用 SystemFileAssociations 按类型挂动词，
  //    不碰 ProgID 和默认打开方式，避免抢走用户的文件关联。
  for (const auto& ext : ArchiveExts()) {
    err = WriteVerb(L"Software\\Classes\\SystemFileAssociations\\" + ext, kVerbExtract,
                    kLabelExtract, exe, kArgExtract);
    if (!err.empty()) return err;
  }

  // 通知资源管理器刷新关联缓存
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return L"";
}

std::wstring UnregisterContextMenu() {
  const std::vector<std::wstring> keys = {
      L"Software\\Classes\\*\\shell\\" + std::wstring(kVerbCompress),
      L"Software\\Classes\\Directory\\shell\\" + std::wstring(kVerbCompress),
  };
  for (const auto& k : keys) DeleteTree(k);

  for (const auto& ext : ArchiveExts()) {
    DeleteTree(L"Software\\Classes\\SystemFileAssociations\\" + ext + L"\\shell\\" +
               kVerbExtract);
  }

  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return L"";
}

bool IsContextMenuRegistered() {
  const std::wstring exe = ExePath();
  if (exe.empty()) return false;

  // 认命令行里的 exe 路径：程序换了位置就视为未注册
  std::wstring cmd;
  if (!ReadString(L"Software\\Classes\\*\\shell\\" + std::wstring(kVerbCompress) + L"\\command",
                  nullptr, cmd)) {
    return false;
  }
  return cmd.find(L"\"" + exe + L"\"") != std::wstring::npos;
}

}  // namespace shell
