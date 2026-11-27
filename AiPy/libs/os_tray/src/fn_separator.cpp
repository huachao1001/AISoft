// `tray.separator()` —— 在右键菜单里插一条分隔线。
//
// 单独一个函数而不是 `tray.item("-", "-")`：菜单里「分隔线」不是一项，它
// 没有 id 也没有回调；用保留 id 表达会让 `tray.on("-", ...)` 这种写法看似
// 合法而实际无效。
#include "aipy/pylib.h"
#include "aipy/os_tray/tray.h"

namespace {

aipy::Value separatorImpl(const aipy::Value*, int) {
  aipy::os_tray::state().items.push_back(
      aipy::os_tray::MenuItem{aipy::os_tray::kSeparatorId, std::string()});
  return aipy::Value();
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_tray, "os.tray", separator, separatorImpl);
