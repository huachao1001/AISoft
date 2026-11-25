// `os.path.getsize(path)` —— 文件字节数。
//
// 对齐 CPython 的 `os.path.getsize`：返回字节数（不是字符数），对目录返回
// 目录项本身的大小。
//
// 与 CPython 的差异只有失败表现：CPython 抛 FileNotFoundError / OSError，
// 这里返回 -1。返回 -1 而不是 0 是关键——0 是一个合法的空文件大小，用 0
// 表示失败会让「空文件」与「读不到」分不开。
#include "aipy/io/paths.h"
#include "aipy/pylib.h"

#include <cstdio>

#if defined(_WIN32)
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

namespace {

aipy::Value getsizeImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value(-1LL);
  const std::string path = args[0].toString();

#if defined(_WIN32)
  const aipy::io::NativePath wide = aipy::io::toNativePath(path);
  if (wide.empty()) return aipy::Value(-1LL);
  struct _stat64 info;
  if (_wstat64(wide.c_str(), &info) != 0) return aipy::Value(-1LL);
  return aipy::Value(static_cast<long long>(info.st_size));
#else
  struct stat info;
  if (stat(path.c_str(), &info) != 0) return aipy::Value(-1LL);
  return aipy::Value(static_cast<long long>(info.st_size));
#endif
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_path, "os.path", getsize, getsizeImpl);
