// 注册表读写的最小封装。
//
// 只碰 `HKCU\Software\Classes`：那是当前用户的类注册区，写它不需要管理员
// 权限，也不会影响别的用户。路径拼装集中在这里，四个导出函数不必各自记
// 「HKCU 加 Software\Classes 加扩展名」这段。
#include "assoc_keys.h"

#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace aipy {
namespace os_fileassoc {

namespace {

#if defined(_WIN32)

std::wstring toWide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()),
                                         nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                      &wide[0], needed);
  return wide;
}

std::string toUtf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return std::string();
  std::string narrow(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                      &narrow[0], needed, nullptr, nullptr);
  return narrow;
}

// 键路径（脚本能看见的那一段，如 `.mydata`）→ 完整宽字符路径。
//
// 所有键都挂在 `Software\Classes` 下，根键固定是 HKEY_CURRENT_USER。集中
// 拼这一处，四个导出函数不必各自重复。
std::wstring fullKeyPath(const std::string& keyPath) {
  return std::wstring(L"Software\\Classes\\") + toWide(keyPath);
}

// 打开（并按需创建）一个键。调用方负责 RegCloseKey。
//
// ── 为什么两条分支必须是不同的 API ──
//
// `RegCreateKeyExW` **没有「只打开」的选项**：它拿到的路径不存在就一定创建，
// `KEY_READ` 权限掩码并不阻止创建。用它实现只读打开的话，一次
// `query(".ext")` 就会把一个刚被撤销的扩展名键重新建出来（空壳），
// 症状是「卸载了但注册表里永远留着垃圾」。这个坑实测踩过，所以只读走
// `RegOpenKeyExW`。
HKEY openKey(const std::string& keyPath, bool create, long* status) {
  HKEY key = nullptr;
  const std::wstring full = fullKeyPath(keyPath);
  long result = 0;
  if (create) {
    result = RegCreateKeyExW(HKEY_CURRENT_USER, full.c_str(), 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                             nullptr, &key, nullptr);
  } else {
    result = RegOpenKeyExW(HKEY_CURRENT_USER, full.c_str(), 0, KEY_READ, &key);
  }
  if (status != nullptr) *status = result;
  return result == ERROR_SUCCESS ? key : nullptr;
}

#endif  // _WIN32

}  // namespace

std::string writeString(const std::string& keyPath, const std::string& name,
                        const std::string& value) {
#if !defined(_WIN32)
  (void)keyPath;
  (void)name;
  (void)value;
  return "fileassoc is only supported on Windows";
#else
  long status = 0;
  HKEY key = openKey(keyPath, true, &status);
  if (key == nullptr) return systemErrorText(status);

  const std::wstring wideName = toWide(name);
  const std::wstring wideValue = toWide(value);
  // REG_SZ 必须带结尾 '\0'，长度按字节算（含 '\0'）。
  const DWORD bytes =
      static_cast<DWORD>((wideValue.size() + 1) * sizeof(wchar_t));
  const long result = RegSetValueExW(
      key, name.empty() ? nullptr : wideName.c_str(), 0, REG_SZ,
      reinterpret_cast<const BYTE*>(wideValue.c_str()), bytes);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS) return systemErrorText(result);
  return std::string();
#endif
}

std::string readString(const std::string& keyPath, const std::string& name) {
#if !defined(_WIN32)
  (void)keyPath;
  (void)name;
  return std::string();
#else
  long status = 0;
  HKEY key = openKey(keyPath, false, &status);
  if (key == nullptr) return std::string();

  const std::wstring wideName = toWide(name);
  DWORD type = 0;
  DWORD bytes = 0;
  // 先问长度：值可能任意长，猜一个缓冲会被截断成空串。
  long result = RegQueryValueExW(key, name.empty() ? nullptr : wideName.c_str(),
                                 nullptr, &type, nullptr, &bytes);
  if (result != ERROR_SUCCESS || type != REG_SZ || bytes == 0) {
    RegCloseKey(key);
    return std::string();
  }

  std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
  result = RegQueryValueExW(key, name.empty() ? nullptr : wideName.c_str(),
                            nullptr, &type,
                            reinterpret_cast<BYTE*>(&buffer[0]), &bytes);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS) return std::string();

  // 去掉结尾的 '\0'：REG_SZ 里一定有它，留着会让比较永远不等。
  while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
  return toUtf8(buffer);
#endif
}

std::string deleteKey(const std::string& keyPath) {
#if !defined(_WIN32)
  (void)keyPath;
  return "fileassoc is only supported on Windows";
#else
  // ── 两步删，缺一不可 ──
  //
  // `RegDeleteTreeW` **只删子键与值，不删这个键自己**（文档明确写的，
  // 实测也是：删完 reg query 仍能打开它，只是里面空了）。留给用户的是
  // 一个空壳扩展名键，资源管理器会把它当成「关联到无」的垃圾项。
  //
  // 所以先递归清空，再删空壳。第一次删不存在时返回 ERROR_FILE_NOT_FOUND，
  // 那是「已经删干净了」，算成功。
  const std::wstring full = fullKeyPath(keyPath);
  long result = RegDeleteTreeW(HKEY_CURRENT_USER, full.c_str());
  if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
    return systemErrorText(result);
  }
  result = RegDeleteKeyW(HKEY_CURRENT_USER, full.c_str());
  if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
    return systemErrorText(result);
  }
  return std::string();
#endif
}

bool keyExists(const std::string& keyPath) {
#if !defined(_WIN32)
  (void)keyPath;
  return false;
#else
  HKEY key = nullptr;
  const long result = RegOpenKeyExW(HKEY_CURRENT_USER,
                                    fullKeyPath(keyPath).c_str(), 0, KEY_READ,
                                    &key);
  if (result != ERROR_SUCCESS) return false;
  RegCloseKey(key);
  return true;
#endif
}

std::string systemErrorText(long code) {
#if !defined(_WIN32)
  (void)code;
  return "fileassoc is only supported on Windows";
#else
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(code),
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::string text;
  if (length != 0 && buffer != nullptr) {
    text = toUtf8(std::wstring(buffer, length));
    LocalFree(buffer);
    // 系统消息带 "\r\n"，去掉——错误描述会拼进一句给脚本读的话里。
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                             text.back() == ' ')) {
      text.pop_back();
    }
  }
  if (text.empty()) text = "registry error " + std::to_string(code);
  return text;
#endif
}

}  // namespace os_fileassoc
}  // namespace aipy
