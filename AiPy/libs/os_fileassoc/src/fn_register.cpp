// `fileassoc.register(ext, prog_id="")` —— 把扩展名关联到当前 exe。
//
// 返回空串表示成功；失败时返回可读原因（脚本可以直接 print 出来）。
//
// 为什么不抛异常：这是 CPython 里根本不存在的操作（`fileassoc` 是 AiPy 的
// 扩展库，不是标准库），没有可对齐的异常类型。返回错误描述让调用方决定
// 「记一条日志继续」还是「当成致命错误」。
#include "aipy/os_fileassoc/fileassoc.h"
#include "aipy/pylib.h"

#include <string>

namespace {

aipy::Value registerImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value("missing extension argument");
  const std::string extension = args[0].toString();
  const std::string progId = count >= 2 ? args[1].toString() : std::string();
  return aipy::Value(aipy::os_fileassoc::registerExtension(extension, progId));
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_fileassoc, "os.fileassoc", register, registerImpl);
