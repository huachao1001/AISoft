// 注册表读写的共用层：路径归一、exe 路径、键操作。
//
// ── 为什么单独一个编译单元 ──
//
// 四个导出函数（register / unregister / icon / query）都用这几个小工具。
// 写在一起会让「只查一下关联」也把写注册表的代码拖进产物。
#include "aipy/os_fileassoc/fileassoc.h"

#include "aipy/vm.h"

#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace aipy {
namespace os_fileassoc {

#if defined(_WIN32)
namespace {

// UTF-8 → UTF-16。注册表 API 全是 W 版本，而脚本里的字符串是 UTF-8。
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

// UTF-16 → UTF-8。
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

}  // namespace
#endif

std::string executablePath() {
#if defined(_WIN32)
  // 宿主已经把真实映像路径放进命令行（launcher 用 GetModuleFileNameW
  // 取的），直接用它——与 `sys.executable()` 同一个来源，两处不会打架。
  const std::vector<std::string>& args = vm::commandLine();
  if (!args.empty() && !args.front().empty()) return args.front();

  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) return std::string();
    if (static_cast<std::size_t>(length) < buffer.size()) {
      return toUtf8(std::wstring(buffer.data(), length));
    }
    // 返回值等于缓冲区大小说明被截断了，扩大后重试。
    buffer.resize(buffer.size() * 2);
  }
#else
  return std::string();
#endif
}

std::string normalizeExtension(const std::string& extension) {
  std::size_t begin = 0;
  std::size_t end = extension.size();
  while (begin < end &&
         (extension[begin] == ' ' || extension[begin] == '\t')) {
    ++begin;
  }
  while (end > begin &&
         (extension[end - 1] == ' ' || extension[end - 1] == '\t')) {
    --end;
  }
  if (begin >= end) return std::string();

  std::string result;
  // 补前导点：`txt` 与 `.txt` 是同一种东西，不该让调用方记两种写法。
  if (extension[begin] != '.') result.push_back('.');
  for (std::size_t i = begin; i < end; ++i) {
    char ch = extension[i];
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    // 路径分隔符、通配符、空白都不能进注册表键名：写进去创建的是一个
    // 永远匹配不上的键，而且不报错。
    if (ch == '\\' || ch == '/' || ch == '*' || ch == '?' || ch == ' ' ||
        ch == '\t') {
      return std::string();
    }
    result.push_back(ch);
  }
  // 只有一个点等于没写扩展名。
  if (result.size() <= 1) return std::string();
  return result;
}

}  // namespace os_fileassoc
}  // namespace aipy
