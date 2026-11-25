// `fs.size(path)` —— 文件字节数，不存在返回 -1。
//
// 与 `os.path.getsize` 同义，但失败用 -1 而不是抛异常。
#include "aipy/fs/io.h"
#include "aipy/pylib.h"

#include <cstdio>

namespace {

aipy::Value sizeImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value(-1LL);
  const std::string path = args[0].toString();

#if defined(_WIN32)
  const std::wstring wide = aipy::fs::toNativePath(path);
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

AIPY_PYLIB_FUNCTION(fs, size, sizeImpl);
