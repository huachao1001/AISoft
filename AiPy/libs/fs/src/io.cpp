// `fs` pylib 的共用底座：路径转换、读写、错误描述。
//
// ── 为什么到处都是宽字符 ──
//
// 脚本里的路径是 UTF-8，而 Windows 的 ANSI API（`fopen`）按**系统代码页**
// 解释。在中文用户名 / 中文目录下，`C:\Users\张三\a.txt` 直接打不开，并且
// 报的错是「找不到文件」——那会把用户引向完全错误的方向。
//
// 所以 Windows 上一律用 `_wfopen`，路径先经 `toNativePath` 转成宽字符。
// 只读文件、不做别的，用不上更重的整套 Win32 文件 API。
//
// ── 为什么错误用返回值表示 ──
//
// AiPy 没有异常通道（见 vm.h 文件头）。扩展失败只能靠返回值：读失败返回
// None、写失败返回 False。这与 `parse_number` 失败返回 None 是同一条约定。
#include "aipy/fs/io.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <sys/stat.h>
#include <windows.h>
#endif

namespace aipy {
namespace fs {
namespace {

// errno → 可读中文原因。
//
// 不直接用 `strerror`：它返回本地化字符串，而这里要的是稳定的说法，
// 好让脚本和测试都能按字面匹配。常见几种手写，其余退回编号。
std::string reasonOf(int code) {
  switch (code) {
    case ENOENT:
      return "文件或目录不存在";
    case EACCES:
      return "没有访问权限";
    case EISDIR:
      return "那是一个目录";
    case ENOTDIR:
      return "路径中有一段不是目录";
    case ENOSPC:
      return "磁盘空间不足";
    case EEXIST:
      return "已经存在";
    case EBUSY:
      return "文件被占用";
    default:
      return "错误码 " + std::to_string(code);
  }
}

}  // namespace

#if defined(_WIN32)

std::wstring toNativePath(const std::string& utf8) {
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

namespace {

std::FILE* openWide(const std::string& path, const wchar_t* mode) {
  const std::wstring wide = toNativePath(path);
  if (wide.empty()) return nullptr;
  return _wfopen(wide.c_str(), mode);
}

}  // namespace

void* openForRead(const std::string& path) { return openWide(path, L"rb"); }
void* openForWrite(const std::string& path, bool append) {
  return openWide(path, append ? L"ab" : L"wb");
}

bool pathExists(const std::string& path, bool* isDirectory) {
  const std::wstring wide = toNativePath(path);
  if (wide.empty()) return false;
  struct _stat64 info;
  if (_wstat64(wide.c_str(), &info) != 0) return false;
  if (isDirectory != nullptr) *isDirectory = (info.st_mode & _S_IFDIR) != 0;
  return true;
}

#else

std::string toNativePath(const std::string& utf8) { return utf8; }

void* openForRead(const std::string& path) { return std::fopen(path.c_str(), "rb"); }
void* openForWrite(const std::string& path, bool append) {
  return std::fopen(path.c_str(), append ? "ab" : "wb");
}

bool pathExists(const std::string& path, bool* isDirectory) {
  struct stat info;
  if (stat(path.c_str(), &info) != 0) return false;
  if (isDirectory != nullptr) *isDirectory = S_ISDIR(info.st_mode);
  return true;
}

#endif

bool readAll(const std::string& path, std::string* content, std::string* error) {
  bool isDirectory = false;
  if (pathExists(path, &isDirectory) && isDirectory) {
    if (error != nullptr) *error = describeError(path, "那是一个目录");
    return false;
  }

  void* handle = openForRead(path);
  if (handle == nullptr) {
    if (error != nullptr) *error = describeError(path, reasonOf(errno));
    return false;
  }

  std::FILE* file = static_cast<std::FILE*>(handle);
  std::string data;
  char chunk[16 * 1024];
  for (;;) {
    const std::size_t got = std::fread(chunk, 1, sizeof(chunk), file);
    if (got == 0) break;
    data.append(chunk, got);
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) {
    if (error != nullptr) *error = describeError(path, "读取中断");
    return false;
  }
  content->swap(data);
  return true;
}

bool writeAll(const std::string& path, const std::string& content, bool append,
              std::string* error) {
  void* handle = openForWrite(path, append);
  if (handle == nullptr) {
    if (error != nullptr) *error = describeError(path, reasonOf(errno));
    return false;
  }
  std::FILE* file = static_cast<std::FILE*>(handle);
  const std::size_t written =
      content.empty() ? 0 : std::fwrite(content.data(), 1, content.size(), file);
  // fclose 的返回值也要看：缓冲区的错误（磁盘满）只在关闭时才报出来。
  const bool closeFailed = std::fclose(file) != 0;
  if (written != content.size() || closeFailed) {
    if (error != nullptr) *error = describeError(path, "写入失败");
    return false;
  }
  return true;
}

std::string describeError(const std::string& path, const std::string& reason) {
  if (path.empty()) return reason;
  return path + ": " + reason;
}

}  // namespace fs
}  // namespace aipy
