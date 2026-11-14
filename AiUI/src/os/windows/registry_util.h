// 注册表读写的共用小工具（Windows 内部使用，不属于公开接口）
//
// 右键菜单注册、文件关联、卸载项登记都要写同一种「键 + 值」，各自复制一份
// 迟早会在错误处理上分叉。这里只放最基础的几个操作，语义保持朴素：
// 写失败返回 false，删不存在的项视为成功。
#pragma once

#include <windows.h>

#include <string>

namespace AiUI {
namespace os {
namespace windows {

// 建键并写一个字符串值；name 为 nullptr 表示写默认值
inline bool regSetString(HKEY root, const std::wstring& subKey, const wchar_t* name,
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

// 递归删除子键；本来就不存在等同于已清理
inline bool regDeleteTree(HKEY root, const std::wstring& subKey) {
  const LONG r = ::RegDeleteTreeW(root, subKey.c_str());
  return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND;
}

// 读一个字符串值；不存在或类型不对返回空
inline std::wstring regGetString(HKEY root, const std::wstring& subKey, const wchar_t* name) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return {};

  wchar_t buffer[MAX_PATH * 2] = {};
  DWORD size = sizeof(buffer);
  const LONG r =
      ::RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(buffer), &size);
  ::RegCloseKey(key);
  return r == ERROR_SUCCESS ? std::wstring(buffer) : std::wstring();
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
