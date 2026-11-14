// 注册表读写的共用小工具（Windows 内部使用，不属于公开接口）
//
// 右键菜单注册、文件关联、卸载项登记都要写同一种「键 + 值」，各自复制一份
// 迟早会在错误处理上分叉。这里只放最基础的几个操作，语义保持朴素：
// 写失败返回 false，删不存在的项视为成功。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

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

  // 先问长度再分配：安装目录可能超过 MAX_PATH，固定缓冲会截断成半条路径，
  // 调用方拿到残缺路径后去删文件是会出事的。
  DWORD type = 0;
  DWORD size = 0;
  std::wstring out;
  if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
      size >= sizeof(wchar_t)) {
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    if (::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()),
                           &size) == ERROR_SUCCESS) {
      out.assign(buffer.data());
    }
  }
  ::RegCloseKey(key);
  return out;
}

// 写一个多字符串值（REG_MULTI_SZ）：用来存「安装时新建的目录层级」。
//
// 为什么用 REG_MULTI_SZ 而不是拼成一个字符串：目录路径本身可以含逗号、分号、
// 竖线，任何自造分隔符都会在某个用户的路径上撞车。REG_MULTI_SZ 是系统自己
// 定义的列表类型，不需要转义。
inline bool regSetMultiString(HKEY root, const std::wstring& subKey, const wchar_t* name,
                              const std::vector<std::wstring>& values) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return false;
  }

  // REG_MULTI_SZ 的布局是「字符串\0字符串\0...\0」，末尾再多一个 \0。
  // 空列表也要写一个结尾的 \0，否则类型不合法。
  std::wstring data;
  for (const auto& value : values) {
    data += value;
    data.push_back(L'\0');
  }
  data.push_back(L'\0');

  const LONG r = ::RegSetValueExW(
      key, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
      static_cast<DWORD>(data.size() * sizeof(wchar_t)));
  ::RegCloseKey(key);
  return r == ERROR_SUCCESS;
}

// 读一个多字符串值；不存在或类型不对返回空表
inline std::vector<std::wstring> regGetMultiString(HKEY root, const std::wstring& subKey,
                                                   const wchar_t* name) {
  std::vector<std::wstring> out;
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return out;

  DWORD type = 0;
  DWORD size = 0;
  if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
      type == REG_MULTI_SZ && size >= sizeof(wchar_t)) {
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    if (::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()),
                           &size) == ERROR_SUCCESS) {
      const wchar_t* p = buffer.data();
      const wchar_t* end = p + size / sizeof(wchar_t);
      while (p < end && *p) {
        const std::wstring item(p);
        if (!item.empty()) out.push_back(item);
        p += item.size() + 1;
      }
    }
  }
  ::RegCloseKey(key);
  return out;
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
