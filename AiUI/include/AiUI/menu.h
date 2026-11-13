// 通用菜单项
//
// 弹出菜单与托盘菜单共用同一份描述，后端各自翻译成系统菜单。
#pragma once

#include <string>

namespace AiUI {

struct MenuItem {
  std::wstring label;
  bool enabled = true;
  // 分隔线；此时 label 被忽略
  bool separator = false;
  // 左侧显示勾选标记；下拉框用它标出当前项
  bool checked = false;
};

}  // namespace AiUI
