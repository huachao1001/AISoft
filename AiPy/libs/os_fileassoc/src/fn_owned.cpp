// `fileassoc.owned(ext)` —— 这个扩展名现在是不是由本程序打开。
//
// 判据是启动命令里的 exe 路径，不是 ProgID 名字：用户可能自己起了别名
// ProgID，但只要它指向本 exe，关联就是我们的。
#include "aipy/os_fileassoc/fileassoc.h"
#include "aipy/pylib.h"

namespace {

aipy::Value ownedImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value(false);
  return aipy::Value(aipy::os_fileassoc::isOwnedByThisExe(args[0].toString()));
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_fileassoc, "os.fileassoc", owned, ownedImpl);
