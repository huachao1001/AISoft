// `os.makedirs(path)` —— 递归创建目录，已存在也算成功。
//
// 对齐 CPython 的 `os.makedirs(path, exist_ok=True)`。
//
// ── 为什么对齐 makedirs 而不是 mkdir ──
//
// 标准 `os.mkdir` 只建**一级**：父目录不存在时报错，且已存在时报错。脚本里
// 最常见的是「确保输出目录在那儿」，用 mkdir 就得自己先递归拆路径——那正是
// makedirs 替你做的事。
//
// 所以这里给的是 makedirs 的语义，但按用户的习惯叫 makedirs（名字即文档）。
// 已存在且是目录 → True；已存在但是文件 → False（不能把文件当目录用）。
//
// 与 CPython 的差异由语言决定：出错返回 False 而不是抛 OSError。
#include "aipy/io/paths.h"
#include "aipy/pylib.h"

#include <string>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace {

// 建一级。已存在返回 true（交给上层判断是不是目录）。
bool makeOne(const std::string& path) {
#if defined(_WIN32)
  const aipy::io::NativePath wide = aipy::io::toNativePath(path);
  if (wide.empty()) return false;
  const int code = _wmkdir(wide.c_str());
#else
  const int code = mkdir(path.c_str(), 0755);
#endif
  // 已存在时底层会失败，但那不是错误——由调用方看 pathExists 判定。
  return code == 0;
}

bool makeAll(const std::string& path) {
  if (path.empty()) return false;

  // 逐段创建：`a/b/c` 先建 `a`、再 `a/b`、最后 `a/b/c`。
  // 先找父路径再递归，比「从根开始拼前缀」少一半的字符串操作。
  bool isDirectory = false;
  if (aipy::io::pathExists(path, &isDirectory)) return isDirectory;

  const std::size_t cut = path.find_last_of("/\\");
  if (cut != std::string::npos && cut > 0) {
    // 盘符根（`C:\`）的 cut 落在 1，此时前缀 `C:` 不是一个可创建的目录。
    const std::string parent = path.substr(0, cut);
    if (!parent.empty() && parent.back() != ':') {
      if (!makeAll(parent)) return false;
    }
  }
  if (makeOne(path)) return true;
  // 并发下可能刚好被别人创建了：再查一次，别把成功报成失败。
  return aipy::io::pathExists(path, &isDirectory) && isDirectory;
}

aipy::Value makedirsImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value(false);
  const std::string path = args[0].toString();
  if (path.empty()) return aipy::Value(false);

  if (!makeAll(path)) {
    aipy::diagnose(aipy::io::describeError(path, "无法创建目录"));
    return aipy::Value(false);
  }
  return aipy::Value(true);
}

}  // namespace

AIPY_PYLIB_FUNCTION(os, makedirs, makedirsImpl);
