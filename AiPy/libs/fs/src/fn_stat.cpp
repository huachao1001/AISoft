// `fs.is_dir(path)` / `fs.is_file(path)` —— 类型判断。
//
// 与 `os.path.isdir` / `os.path.isfile` 同义。两个函数共用一个文件：
// 它们只是同一个 stat 结果的两种解读，拆开反而让「判目录」和「判文件」
// 分两处实现、容易不一致。
#include "aipy/fs/io.h"
#include "aipy/pylib.h"

namespace {

// 是否存在且类型匹配。exists 与 isDirectory 一次拿到，避免两次 stat。
bool matches(const aipy::Value* args, int count, bool wantDirectory) {
  if (count < 1) return false;
  bool isDirectory = false;
  if (!aipy::fs::pathExists(args[0].toString(), &isDirectory)) return false;
  return isDirectory == wantDirectory;
}

aipy::Value isDirImpl(const aipy::Value* args, int count) {
  return aipy::Value(matches(args, count, true));
}

aipy::Value isFileImpl(const aipy::Value* args, int count) {
  return aipy::Value(matches(args, count, false));
}

}  // namespace

// 两个符号名指向同一个编译单元：函数级按需的粒度是 .obj，而这两个函数
// 只有几行、且永远一起被想到，分开不值得多一个 obj。
AIPY_PYLIB_FUNCTION(fs, is_dir, isDirImpl);
AIPY_PYLIB_FUNCTION(fs, is_file, isFileImpl);
