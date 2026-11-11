// LayoutInflater：按 XML 文件名取到构建期生成的布局类
//
// 用法上像"渲染 main_frame.xml"，实际不做任何 XML 解析——每个 XML 在构建期
// 已由 layoutgen 编译成 C++ 类，并注册成"名字 → 工厂"的映射。
// 这样既保留 XML 描述布局的写法，又不给运行时增加解析开销。
//
// 新增 XML 后由 aiui_add_layouts() 自动补充注册项，调用方不用改代码。
#pragma once

#include "AiUI/view_group.h"

#include <string>

namespace AiUI {

class LayoutInflater {
 public:
  // 布局工厂：每次调用都建出一棵新树
  using Factory = ViewGroup* (*)();

  // 按名字建出布局树；名字是 XML 文件名去掉扩展名（main_frame）。
  // 找不到返回 nullptr。返回对象由调用方持有，通常交给 Window::setContentView。
  static ViewGroup* inflate(const std::wstring& name);

  // 是否注册过这个名字
  static bool has(const std::wstring& name);

  // 注册布局；由构建期生成的代码在静态初始化阶段调用，业务代码不需要碰
  static void add(const std::wstring& name, Factory factory);
};

// 生成的注册项：声明一个静态对象，构造时把布局登记进去
class LayoutRegistration {
 public:
  LayoutRegistration(const std::wstring& name, LayoutInflater::Factory factory) {
    LayoutInflater::add(name, factory);
  }
};

}  // namespace AiUI
