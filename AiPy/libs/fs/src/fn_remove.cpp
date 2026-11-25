// `fs.remove(path)` —— 删除文件，返回是否成功。
//
// 对应 `os.remove`。只删文件不删目录：删目录要递归，语义完全不同，
// 不该藏在同一个名字后面（用户在两者之间搞混时损失可能很大）。
#include "aipy/fs/io.h"
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
  if (!aipy::fs::pathExists(path, &isDirectory)) return aipy::Value(false);
  if (isDirectory) {
    // 明确拒绝，而不是让底层报一个看不懂的错误。
    aipy::diagnose(aipy::fs::describeError(path, "那是一个目录，remove 只删文件"));
    return aipy::Value(false);
  }

#if defined(_WIN32)
  const std::wstring wide = aipy::fs::toNativePath(path);
  if (wide.empty()) return aipy::Value(false);
  if (_wremove(wide.c_str()) != 0) {
    aipy::diagnose(aipy::fs::describeError(path, "删除失败"));
    return aipy::Value(false);
  }
#else
  if (std::remove(path.c_str()) != 0) {
    aipy::diagnose(aipy::fs::describeError(path, "删除失败"));
    return aipy::Value(false);
  }
#endif
  return aipy::Value(true);
}

}  // namespace

AIPY_PYLIB_FUNCTION(fs, remove, removeImpl);
