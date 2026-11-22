// 通用菜单模型
//
// 弹出菜单（应用内右键）与系统右键菜单、托盘菜单共用这一份描述。
// 后端各自把它翻译成系统菜单（Win32 HMENU / NSMenu / GtkMenu），
// 应用侧只产出纯数据的菜单模型，不接触任何平台机制。
#pragma once

#include "pyui/types.h"

#include <string>
#include <vector>

namespace pyui {

struct MenuItem {
  // 稳定标识，用于回传选中结果。分隔线忽略。
  // 用字符串而不是下标，是因为菜单内容会动态变化（如「压缩到 rnd.zip」），
  // 下标在两次弹出之间没有可比性。
  std::string id;
  std::string label;
  bool enabled = true;
  bool separator = false;
  bool checked = false;
  // 是否置顶到菜单最前；false 时按 id 排序，同前缀的动作自然相邻
  bool topLevel = false;
  // 子菜单。为空表示叶子项。
  std::vector<MenuItem> children;
  // 附加参数：动态菜单用它把「压缩到 xxx.zip」这类文案与命令解耦
  std::string command;

  static MenuItem separatorItem() {
    MenuItem item;
    item.separator = true;
    item.enabled = false;
    return item;
  }
  static MenuItem action(std::string id, std::string label, std::string command = {}) {
    MenuItem item;
    item.id = std::move(id);
    item.label = std::move(label);
    item.command = std::move(command);
    return item;
  }
};

// 一次弹出菜单的请求。屏幕坐标用 Point（后端负责按平台缩放）。
struct PopupMenuRequest {
  NativeHandle owner = nullptr;
  Point screenPosition{};
  std::vector<MenuItem> items;
  // 是否显示图标列。系统菜单会自动对齐，自绘菜单用它决定是否留白。
  bool showIcons = false;
};

}  // namespace pyui
