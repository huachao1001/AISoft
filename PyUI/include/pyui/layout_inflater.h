// LayoutInflater：按布局名取到构建期生成的控件树
//
// 用法上像"加载 main_frame.xml"，实际不做任何解析——每个 XML 在构建期已由
// pyui_layoutc 编译成 C++ 工厂函数，并注册成"名字 → 工厂"的映射。
//
// 关键：**开发者拿到的只有控件树，没有生成类**。
//   auto root = LayoutInflater::inflate("main_frame");
//   auto* btn = root->findView<Button>("ok");
// 这里没有 MainFrame 这个类型，也没有 root->crumb 这种成员。所以
// 「布局编译成 C++ 还是资源文件」对业务代码完全不可见——将来换实现，
// 这一行不用改（见 AGENTS.md 铁律四）。
//
// 新增 XML 后由 pyui_add_layouts() 自动补充注册项，调用方不用改代码。
#pragma once

#include "pyui/view_group.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pyui {

class LayoutInflater {
 public:
  // 布局工厂：每次调用都建出一棵新树，所有权交给调用方
  using Factory = ViewGroup* (*)();

  // 按名字建出布局树；名字是 XML 文件名去掉扩展名（main_frame）。
  // 找不到返回 nullptr。
  static std::unique_ptr<ViewGroup> inflate(const std::string& name);

  // 是否注册过这个名字
  static bool has(const std::string& name);

  // 所有已注册的布局名。诊断与设计预览用。
  static std::vector<std::string> names();

  // 注册布局；由构建期生成的代码在静态初始化阶段调用，业务代码不需要碰
  static void add(const std::string& name, Factory factory);
};

// 生成的注册项：声明一个静态对象，构造时把布局登记进去
class LayoutRegistration {
 public:
  LayoutRegistration(const std::string& name, LayoutInflater::Factory factory) {
    LayoutInflater::add(name, factory);
  }
};

// 注册内置布局（标题栏等）。
//
// 为什么不只靠静态注册项：内置布局编译进 pyui_core，而 pyui_core 是静态
// 库——没有人引用的目标文件会被链接器整块丢弃，注册项随之消失，症状是
// 「标题栏是空的」而不是链接错误。显式调用一次是最省事的保证。
//
// 幂等，多次调用没有副作用。宿主在创建窗口之前调一次即可。
void registerBuiltinLayouts();

}  // namespace pyui
