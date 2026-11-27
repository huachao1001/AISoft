// `fileassoc.icon(ext, ico_path="")` —— 给关联设置图标。
//
// 空路径表示退回 exe 自带的图标（第 0 个资源）。只能给**已注册**的扩展名
// 设图标：没有 ProgID 就没有 `DefaultIcon` 键可写。
#include "aipy/os_fileassoc/fileassoc.h"
#include "aipy/pylib.h"

#include <string>

namespace {

aipy::Value iconImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value("missing extension argument");
  const std::string extension = args[0].toString();
  const std::string iconPath = count >= 2 ? args[1].toString() : std::string();
  return aipy::Value(aipy::os_fileassoc::setIcon(extension, iconPath));
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_fileassoc, "os.fileassoc", icon, iconImpl);
