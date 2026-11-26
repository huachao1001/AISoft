// 路径与换行的共用实现。
//
// 见 paths.h 头部：这里只有「不需要打开文件」的那部分。文件读写留在
// fs 库里，否则任何只想判个存不存在的产物都要带一整套读写缓冲。
#include "aipy/io/paths.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aipy {
namespace io {

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

// 系统错误码 → 异常类名。脚本里 `except FileNotFoundError:` 要能接住，
// 所以库层的失败必须抛语言认得的类型。
const char* exceptionTypeOf(int code) {
  switch (code) {
    case ENOENT:
      return "FileNotFoundError";
    case EACCES:
    case EPERM:
      return "PermissionError";
    case EEXIST:
      return "FileExistsError";
    case EISDIR:
      return "IsADirectoryError";
    default:
      return "OSError";
  }
}

#if defined(_WIN32)

NativePath toNativePath(const std::string& utf8) {
  if (utf8.empty()) return NativePath();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()),
                                         nullptr, 0);
  if (needed <= 0) return NativePath();
  NativePath wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                      &wide[0], needed);
  return wide;
}

bool pathExists(const std::string& path, bool* isDirectory) {
  const NativePath wide = toNativePath(path);
  if (wide.empty()) return false;
  struct _stat64 info;
  if (_wstat64(wide.c_str(), &info) != 0) return false;
  if (isDirectory != nullptr) *isDirectory = (info.st_mode & _S_IFDIR) != 0;
  return true;
}

#else

NativePath toNativePath(const std::string& utf8) { return utf8; }

bool pathExists(const std::string& path, bool* isDirectory) {
  struct stat info;
  if (stat(path.c_str(), &info) != 0) return false;
  if (isDirectory != nullptr) *isDirectory = S_ISDIR(info.st_mode);
  return true;
}

#endif

std::string describeError(const std::string& path, const std::string& reason) {
  if (path.empty()) return reason;
  return path + ": " + reason;
}

const char* platformNewline() {
#if defined(_WIN32)
  return "\r\n";
#else
  return "\n";
#endif
}

std::string toPlatformNewlines(const std::string& text) {
#if defined(_WIN32)
  // 非 Windows 上是恒等变换，不必走一遍拷贝。
  std::string out;
  out.reserve(text.size() + text.size() / 16);
  for (char ch : text) {
    if (ch == '\n') out.append("\r\n");
    else out.push_back(ch);
  }
  return out;
#else
  return text;
#endif
}

std::string normalizeNewlines(const std::string& raw) {
  // 一轮扫描：`\r\n` 收成一个 `\n`，孤立的 `\r` 也收成 `\n`。
  // 先查有没有 `\r`，没有就整段复制——文本模式读大文件时这省掉一次
  // 逐字节的分支。
  if (raw.find('\r') == std::string::npos) return raw;

  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const char ch = raw[i];
    if (ch == '\r') {
      out.push_back('\n');
      // `\r\n` 里的 `\n` 已被上面这个 `\n` 代表，跳过它。
      if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

}  // namespace io
}  // namespace aipy
