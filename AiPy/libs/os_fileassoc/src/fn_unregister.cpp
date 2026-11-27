// `fileassoc.unregister(ext)` —— 撤销关联。
//
// 只删自己写的那几个键。扩展名已经被别的 ProgID 占用时什么也不做、返回
// 空串：卸载自己的程序不该顺手拆掉别人（或用户自己在「打开方式」里选的）
// 关联。
#include "aipy/os_fileassoc/fileassoc.h"
#include "aipy/pylib.h"

#include <string>

namespace {

aipy::Value unregisterImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value("missing extension argument");
  return aipy::Value(
      aipy::os_fileassoc::unregisterExtension(args[0].toString()));
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_fileassoc, "os.fileassoc", unregister, unregisterImpl);
