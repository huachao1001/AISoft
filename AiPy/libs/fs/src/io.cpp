// `fs` pylib 的读写实现。
//
// 路径转换、存在性、换行归一、错误描述都在 aipy/io/paths.h：那些 `os` 与
// `os.path` 也要用，只有「真的打开并读写文件」是这里独有的。
//
// ── 为什么错误用返回值表示 ──
//
// AiPy 没有异常通道（见 vm.h 文件头）。扩展失败只能靠返回值：读失败返回
// None、写失败返回 False。这与 `parse_number` 失败返回 None 是同一条约定。
#include "aipy/fs/io.h"

#include <cerrno>
#include <cstdio>

#include "aipy/io/paths.h"

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

using aipy::io::NativePath;
using aipy::io::toNativePath;

#if defined(_WIN32)

std::FILE* openWide(const std::string& path, const wchar_t* mode) {
  const NativePath wide = toNativePath(path);
  if (wide.empty()) return nullptr;
  return _wfopen(wide.c_str(), mode);
}

// 用 CRT 的 _wsopen_s 走底层 open，才能拿到 O_EXCL 语义：
// `_wfopen(path, L"wb")` 遇到已存在的文件会静默截断，而 Python 的 "x"
// 模式必须失败。
//
// mode 必须与 flags 相容：_fdopen 的 "w" 会截断、且与 _O_APPEND 冲突，
// 所以三者由调用方成对给出，而不是在这里猜。
std::FILE* openWideFlags(const std::string& path, int flags,
                         const wchar_t* mode) {
  const NativePath wide = toNativePath(path);
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

#else

std::FILE* openNarrowFlags(const std::string& path, int flags,
                           const char* mode) {
  const int fd = ::open(path.c_str(), flags, 0666);
  if (fd < 0) return nullptr;
  std::FILE* file = ::fdopen(fd, mode);
  if (file == nullptr) ::close(fd);
  return file;
}

#endif

}  // namespace

// 四个打开函数必须是 aipy::fs 的成员：它们声明在 io.h 里，供 fn_open.cpp
// 调用。放进匿名命名空间会让外部看不到它们（而且会遮住外部声明）。

#if defined(_WIN32)

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

#else

void* openForRead(const std::string& path) {
  return std::fopen(path.c_str(), "rb");
}
void* openForWrite(const std::string& path, bool append) {
  return std::fopen(path.c_str(), append ? "ab" : "wb");
}

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

#endif

bool readAll(const std::string& path, std::string* content, std::string* error,
             bool text, int* errorCode) {
  if (errorCode != nullptr) *errorCode = 0;
  bool isDirectory = false;
  if (aipy::io::pathExists(path, &isDirectory) && isDirectory) {
    if (error != nullptr) *error = aipy::io::describeError(path, "那是一个目录");
    // CPython 打开目录是 PermissionError（EACCES），不是 IsADirectoryError。
    if (errorCode != nullptr) *errorCode = EACCES;
    return false;
  }

  errno = 0;
  void* handle = openForRead(path);
  if (handle == nullptr) {
    const int code = errno == 0 ? EIO : errno;
    if (error != nullptr) {
      *error = aipy::io::describeError(path, aipy::io::reasonOf(code));
    }
    if (errorCode != nullptr) *errorCode = code;
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
    if (error != nullptr) {
      *error = aipy::io::describeError(path, "读取中断");
    }
    if (errorCode != nullptr) *errorCode = EIO;
    return false;
  }
  if (text) data = aipy::io::normalizeNewlines(data);
  content->swap(data);
  return true;
}

bool writeAll(const std::string& path, const std::string& content, bool append,
              std::string* error, bool text, int* errorCode) {
  if (errorCode != nullptr) *errorCode = 0;
  errno = 0;
  void* handle = openForWrite(path, append);
  if (handle == nullptr) {
    const int code = errno == 0 ? EIO : errno;
    if (error != nullptr) {
      *error = aipy::io::describeError(path, aipy::io::reasonOf(code));
    }
    if (errorCode != nullptr) *errorCode = code;
    return false;
  }
  std::FILE* file = static_cast<std::FILE*>(handle);
  const std::string payload =
      text ? aipy::io::toPlatformNewlines(content) : content;
  const std::size_t written =
      payload.empty()
          ? 0
          : std::fwrite(payload.data(), 1, payload.size(), file);
  // fclose 的返回值也要看：缓冲区的错误（磁盘满）只在关闭时才报出来。
  const bool closeFailed = std::fclose(file) != 0;
  if (written != payload.size() || closeFailed) {
    if (error != nullptr) *error = aipy::io::describeError(path, "写入失败");
    if (errorCode != nullptr) *errorCode = errno == 0 ? EIO : errno;
    return false;
  }
  return true;
}

}  // namespace fs
}  // namespace aipy
