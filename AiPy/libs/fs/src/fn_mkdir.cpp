// `fs.mkdir(path)` —— 创建目录，返回是否成功。
//
// 对应 `os.makedirs(path, exist_ok=True)` 的语义：已存在也算成功。
//
// 为什么不用 `os.mkdir` 的「已存在就报错」：脚本里创建输出目录最常见的
// 写法就是「确保它在那儿」，每次都要先 exists 再 mkdir 是多余的两步。
#include "aipy/fs/io.h"
#include "aipy/pylib.h"

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

aipy::Value mkdirImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value(false);
  const std::string path = args[0].toString();

  bool isDirectory = false;
  if (aipy::fs::pathExists(path, &isDirectory)) {
    // 已存在：是目录就算成功，是文件就是失败（不能把文件当目录用）。
    return aipy::Value(isDirectory);
  }

#if defined(_WIN32)
  const std::wstring wide = aipy::fs::toNativePath(path);
  if (wide.empty()) return aipy::Value(false);
  const int code = _wmkdir(wide.c_str());
#else
  const int code = mkdir(path.c_str(), 0755);
#endif
  if (code != 0) {
    aipy::diagnose(aipy::fs::describeError(path, "无法创建目录"));
    return aipy::Value(false);
  }
  return aipy::Value(true);
}

}  // namespace

AIPY_PYLIB_FUNCTION(fs, mkdir, mkdirImpl);
