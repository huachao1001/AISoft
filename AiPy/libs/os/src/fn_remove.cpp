// `os.remove(path)` —— 删除文件，返回是否成功。
//
// 对齐 CPython 的 `os.remove`：只删文件不删目录。
//
// 为什么拒绝目录而不是让底层报一个看不懂的错误：`remove` 与 `rmdir` 在用户
// 心里经常搞混，而「删掉一个目录」可能的损失比删错文件大得多。明确说
// 「那是一个目录」比抛一个 EACCES 有用。
//
// 与 CPython 的差异由语言决定：失败返回 False 而不是抛 OSError。
#include "aipy/io/paths.h"
#include "aipy/pylib.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

aipy::Value removeImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value(false);
  const std::string path = args[0].toString();

  bool isDirectory = false;
  if (!aipy::io::pathExists(path, &isDirectory)) return aipy::Value(false);
  if (isDirectory) {
    aipy::diagnose(
        aipy::io::describeError(path, "那是一个目录，remove 只删文件"));
    return aipy::Value(false);
  }

#if defined(_WIN32)
  const aipy::io::NativePath wide = aipy::io::toNativePath(path);
  if (wide.empty()) return aipy::Value(false);
  if (_wremove(wide.c_str()) != 0) {
    aipy::diagnose(aipy::io::describeError(path, "删除失败"));
    return aipy::Value(false);
  }
#else
  if (std::remove(path.c_str()) != 0) {
    aipy::diagnose(aipy::io::describeError(path, "删除失败"));
    return aipy::Value(false);
  }
#endif
  return aipy::Value(true);
}

}  // namespace

AIPY_PYLIB_FUNCTION(os, remove, removeImpl);
