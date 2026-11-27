// `fileassoc.query(ext)` —— 扩展名现在关联到哪个 ProgID。
//
// 返回空串表示没有关联。给脚本判断「装没装上」用，不必自己去读注册表。
#include "aipy/os_fileassoc/fileassoc.h"
#include "aipy/pylib.h"

#include <string>

namespace {

aipy::Value queryImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value(std::string());
  return aipy::Value(aipy::os_fileassoc::queryExtension(args[0].toString()));
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_fileassoc, "os.fileassoc", query, queryImpl);
