// `fs.exists(path)` —— 文件或目录是否存在。
//
// 对应 Python 的 `os.path.exists`。存在但没权限时也返回 False，与
// CPython 在多数情况下的表现一致（宁可说「没有」也不误报）。
#include "aipy/fs/io.h"
#include "aipy/pylib.h"

namespace {

aipy::Value existsImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value(false);
  return aipy::Value(aipy::fs::pathExists(args[0].toString(), nullptr));
}

}  // namespace

AIPY_PYLIB_FUNCTION(fs, exists, existsImpl);
