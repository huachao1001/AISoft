// `fs` pylib 的共用底座：路径转换、读写、换行、错误描述。
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

// 用 CRT 的 _wsopen_s 走底层 open，才能拿到 O_EXCL 语义：
// `_wfopen(path, L"wb")` 遇到已存在的文件会静默截断，而 Python 的 "x"
// 模式必须失败。
//
// mode 必须与 flags 相容：_fdopen 的 "w" 会截断、且与 _O_APPEND 冲突，
// 所以三者由调用方成对给出，而不是在这里猜。
std::FILE* openWideFlags(const std::string& path, int flags, const wchar_t* mode) {
  const std::wstring wide = toNativePath(path);
  if (wide.empty()) return nullptr;
  int fd = -1;
  const errno_t code = _wsopen_s(&fd, wide.c_str(), flags, _SH_DENYNO,
                                 _S_IREAD | _S_IWRITE);
  if (code != 0 || fd < 0) {
    errno = static_cast<int>(code);
    return nullptr;
  }
  std::FILE* file = _wfdopen(fd, mode);
  if (file == nullptr) {
    _close(fd);
    return nullptr;
  }
  return file;
}

}  // namespace

void* openForRead(const std::string& path) { return openWide(path, L"rb"); }
void* openForWrite(const std::string& path, bool append) {
  return openWide(path, append ? L"ab" : L"wb");
}

void* openExclusive(const std::string& path, bool plus) {
  int flags = _O_CREAT | _O_EXCL | _O_BINARY;
  flags |= plus ? _O_RDWR : _O_WRONLY;
  // 文件刚由 O_EXCL 创建、必定是空的，所以读句柄用 "rb+" 不会丢内容；
  // 写成 "w" 会让 _wfdopen 有机会按自己的理解截断。
  return openWideFlags(path, flags, plus ? L"rb+" : L"wb");
}

void* openForUpdate(const std::string& path, bool append) {
  int flags = append ? (_O_CREAT | _O_APPEND) : 0;
  flags |= _O_RDWR | _O_BINARY;
  return openWideFlags(path, flags, append ? L"ab+" : L"rb+");
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

namespace {

std::FILE* openNarrowFlags(const std::string& path, int flags, const char* mode) {
  const int fd = ::open(path.c_str(), flags, 0666);
  if (fd < 0) return nullptr;
  std::FILE* file = ::fdopen(fd, mode);
  if (file == nullptr) ::close(fd);
  return file;
}

}  // namespace

void* openExclusive(const std::string& path, bool plus) {
  int flags = O_CREAT | O_EXCL;
  flags |= plus ? O_RDWR : O_WRONLY;
  return openNarrowFlags(path, flags, plus ? "rb+" : "wb");
}

void* openForUpdate(const std::string& path, bool append) {
  int flags = append ? (O_CREAT | O_APPEND) : 0;
  flags |= O_RDWR;
  return openNarrowFlags(path, flags, append ? "ab+" : "rb+");
}

bool pathExists(const std::string& path, bool* isDirectory) {
  struct stat info;
  if (stat(path.c_str(), &info) != 0) return false;
  if (isDirectory != nullptr) *isDirectory = S_ISDIR(info.st_mode);
  return true;
}

#endif

const char* platformNewline() {
#if defined(_WIN32)
  return "\r\n";
#else
  return "\n";
#endif
}

std::string normalizeNewlines(std::string_view raw) {
  // 一轮扫描：`\r\n` 收成一个 `\n`，孤立的 `\r` 也收成 `\n`。
  // 先查有没有 `\r`，没有就整段复制——文本模式读大文件时这省掉一次
  // 逐字节的分支。
  if (raw.find('\r') == std::string_view::npos) return std::string(raw);

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

std::string toPlatformNewlines(std::string_view text) {
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
  return std::string(text);
#endif
}

bool readAll(const std::string& path, std::string* content, std::string* error,
             bool text) {
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
  if (text) data = normalizeNewlines(data);
  content->swap(data);
  return true;
}

bool writeAll(const std::string& path, const std::string& content, bool append,
              std::string* error, bool text) {
  void* handle = openForWrite(path, append);
  if (handle == nullptr) {
    if (error != nullptr) *error = describeError(path, reasonOf(errno));
    return false;
  }
  std::FILE* file = static_cast<std::FILE*>(handle);
  const std::string payload =
      text ? toPlatformNewlines(content) : content;
  const std::size_t written = payload.empty()
                                  ? 0
                                  : std::fwrite(payload.data(), 1, payload.size(), file);
  // fclose 的返回值也要看：缓冲区的错误（磁盘满）只在关闭时才报出来。
  const bool closeFailed = std::fclose(file) != 0;
  if (written != payload.size() || closeFailed) {
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
