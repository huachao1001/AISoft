// LayoutInflater 实现：名字到布局工厂的注册表
//
// 表用函数内静态保存，避免静态初始化顺序问题：生成的注册对象在别的翻译
// 单元里构造，可能早于这里的全局对象。
#include "AiUI/layout_inflater.h"

#include <map>
#include <string>

namespace AiUI {
namespace {

std::map<std::wstring, LayoutInflater::Factory>& registry() {
  static std::map<std::wstring, LayoutInflater::Factory> table;
  return table;
}

}  // namespace

void LayoutInflater::add(const std::wstring& name, Factory factory) {
  if (name.empty() || !factory) return;
  registry()[name] = factory;
}

ViewGroup* LayoutInflater::inflate(const std::wstring& name) {
  const auto& table = registry();
  const auto it = table.find(name);
  return it == table.end() ? nullptr : it->second();
}

bool LayoutInflater::has(const std::wstring& name) {
  const auto& table = registry();
  return table.find(name) != table.end();
}

}  // namespace AiUI
