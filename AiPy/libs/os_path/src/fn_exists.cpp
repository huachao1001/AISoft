// `os.path.exists(path)` —— 路径是否存在。
//
// 对齐 CPython 的 `os.path.exists`：文件或目录都算存在；没有权限时返回
// False 而不是报错（CPython 在多数情况下也是这样——宁可说「没有」也不误报）。
//
// 与 CPython 的差异：CPython 对「路径含非法字符」返回 False，这里同样
// 返回 False，行为一致。
#include "aipy/io/paths.h"
#include "aipy/pylib.h"

namespace {

aipy::Value existsImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value(false);
  return aipy::Value(aipy::io::pathExists(args[0].toString(), nullptr));
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_path, "os.path", exists, existsImpl);
