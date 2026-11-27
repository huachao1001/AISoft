// `tray.item(id, label)` —— 往右键菜单加一项。
//
// 顺序就是加入顺序。同一个 id 重复加入时替换文本而不是追加：脚本重跑一遍
// 配置（比如改语言）时不会留下重复项。
//
// 分隔线用单独的 `tray.separator()`：它要能独立按需链接，不能塞在这里。
#include "aipy/pylib.h"
#include "aipy/os_tray/tray.h"

#include <string>
#include <vector>

namespace {

aipy::Value itemImpl(const aipy::Value* args, int count) {
  if (count < 2) return aipy::Value(false);
  const std::string id = args[0].toString();
  const std::string label = args[1].toString();
  if (id.empty()) return aipy::Value(false);
  // 分隔线的 id 是保留的，走 tray.separator() 才不会被当成菜单项。
  if (id == aipy::os_tray::kSeparatorId) return aipy::Value(false);

  std::vector<aipy::os_tray::MenuItem>& items = aipy::os_tray::state().items;
  for (aipy::os_tray::MenuItem& existing : items) {
    if (existing.id != id) continue;
    existing.label = label;
    return aipy::Value(true);
  }
  items.push_back(aipy::os_tray::MenuItem{id, label});
  return aipy::Value(true);
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_tray, "os.tray", item, itemImpl);
