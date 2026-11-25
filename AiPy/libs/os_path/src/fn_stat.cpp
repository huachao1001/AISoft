// `os.path.isfile(path)` / `os.path.isdir(path)` —— 类型判断。
//
// 对齐 CPython：路径不存在、或类型不匹配，都返回 False（不报错）。
//
// 两个函数共用一个编译单元：它们只是同一个 stat 结果的两种解读，拆开反而
// 让「判目录」和「判文件」分两处实现、容易不一致。代价是只用一个会连带
// 另一个进来——两个函数加起来不到 200 字节，不值得为此拆文件。
#include "aipy/io/paths.h"
#include "aipy/pylib.h"

namespace {

// 是否存在且类型匹配。exists 与 isDirectory 一次拿到，避免两次 stat。
bool matches(const aipy::Value* args, int count, bool wantDirectory) {
  if (count < 1) return false;
  bool isDirectory = false;
  if (!aipy::io::pathExists(args[0].toString(), &isDirectory)) return false;
  return isDirectory == wantDirectory;
}

aipy::Value isdirImpl(const aipy::Value* args, int count) {
  return aipy::Value(matches(args, count, true));
}

aipy::Value isfileImpl(const aipy::Value* args, int count) {
  return aipy::Value(matches(args, count, false));
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_path, "os.path", isdir, isdirImpl);
AIPY_PYLIB_FUNCTION_NAMED(os_path, "os.path", isfile, isfileImpl);
