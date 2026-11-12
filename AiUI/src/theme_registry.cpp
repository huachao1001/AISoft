// ThemeRegistry 实现：进程内一张名字 → 调色板 的表
#include "AiUI/theme_registry.h"

#include <map>

namespace AiUI {

namespace {

std::map<std::wstring, const Theme*>& registry() {
  static std::map<std::wstring, const Theme*> table;
  return table;
}

}  // namespace

void ThemeRegistry::add(const std::wstring& name, const Theme* theme) {
  if (!theme) return;
  registry()[name] = theme;
}

const Theme* ThemeRegistry::find(const std::wstring& name) {
  auto& table = registry();
  auto it = table.find(name);
  return it == table.end() ? nullptr : it->second;
}

const Theme& ThemeRegistry::getOr(const std::wstring& name, const Theme& fallback) {
  const Theme* found = find(name);
  return found ? *found : fallback;
}

bool ThemeRegistry::has(const std::wstring& name) { return registry().count(name) > 0; }

}  // namespace AiUI
