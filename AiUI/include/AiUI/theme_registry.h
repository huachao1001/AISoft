// ThemeRegistry：按名字取调色板
//
// theme.xml 在构建期被编译成一组命名 Theme（静态对象），并在这里登记。
// 运行时用 setTheme("dark") 之类的名字切换整棵树，无需重建控件。
//
// 名字用 std::wstring 是为了和布局/资源系统的其它入口保持一致。
#pragma once

#include "AiUI/theme.h"

#include <string>

namespace AiUI {

class ThemeRegistry {
 public:
  // 登记一个调色板；对象由调用方持有（通常是静态对象），注册表只存指针。
  // 同名会覆盖，后注册者生效。
  static void add(const std::wstring& name, const Theme* theme);

  // 按名字查；找不到返回 nullptr
  static const Theme* find(const std::wstring& name);

  // 查不到时回退到浅色主题，适合直接接到 Window::setAppTheme
  static const Theme& getOr(const std::wstring& name, const Theme& fallback);

  // 是否登记过这个名字
  static bool has(const std::wstring& name);
};

// 由构建期生成的主题代码在静态初始化阶段调用
class ThemeRegistration {
 public:
  ThemeRegistration(const std::wstring& name, const Theme* theme) {
    ThemeRegistry::add(name, theme);
  }
};

}  // namespace AiUI
