#include "pyui/layout_inflater.h"

#include <map>
#include <mutex>

namespace pyui {
namespace {

// 名字 → 工厂。用 map 而不是 unordered_map：布局只有几十个，顺序遍历更省内存，
// 而且 names() 需要一个稳定顺序（诊断输出与设计预览要可复现）。
using Registry = std::map<std::string, LayoutInflater::Factory>;

Registry& registry() {
  // 函数内静态：避免静态初始化顺序问题——生成代码的注册项在任何
  // 其它静态对象之前/之后构造都能找到同一个表。
  static Registry table;
  return table;
}

// 注册项在静态初始化阶段写入，与业务代码的 inflate 可能跨线程（测试里常见），
// 所以加锁。只在启动期有写入，运行期是一次无锁读（见下方 cached 说明）。
std::mutex& registryMutex() {
  static std::mutex mutex;
  return mutex;
}

}  // namespace

void LayoutInflater::add(const std::string& name, Factory factory) {
  if (name.empty() || factory == nullptr) return;
  std::lock_guard<std::mutex> lock(registryMutex());
  registry()[name] = factory;
}

bool LayoutInflater::has(const std::string& name) {
  std::lock_guard<std::mutex> lock(registryMutex());
  return registry().find(name) != registry().end();
}

std::unique_ptr<ViewGroup> LayoutInflater::inflate(const std::string& name) {
  Factory factory = nullptr;
  {
    std::lock_guard<std::mutex> lock(registryMutex());
    const auto it = registry().find(name);
    if (it == registry().end()) return nullptr;
    factory = it->second;
  }

  // 在锁外调用：工厂里可能又会调 inflate（布局里嵌套复用另一个布局），
  // 持锁重入会死锁。
  ViewGroup* root = factory();
  return std::unique_ptr<ViewGroup>(root);
}

std::vector<std::string> LayoutInflater::names() {
  std::lock_guard<std::mutex> lock(registryMutex());
  std::vector<std::string> out;
  out.reserve(registry().size());
  for (const auto& kv : registry()) out.push_back(kv.first);
  return out;
}

}  // namespace pyui
