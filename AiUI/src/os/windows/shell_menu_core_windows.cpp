// 菜单逻辑模块的导出入口（Windows）
//
// 应用只实现 aiuiBuildShellMenu（纯计算），这里把它包成一套纯 C 导出：
//   AiuiShellMenuCoreVersion → 接口版本
//   AiuiBuildShellMenuCore   → 按选择算出菜单项，写进调用方给的定长缓冲
//
// 为什么是定长缓冲而不是返回容器：本模块与外壳各有一份静态 CRT，跨 dll
// 释放对方分配的内存会崩资源管理器。谁分配谁释放，边界只有裸指针与整数。
//
// 本模块每次弹菜单才被加载、用完立刻卸载，所以「改完文案换掉文件即生效」。
#include "AiUI/shell_menu.h"
#include "AiUI/shell_menu_core.h"

#include <string>
#include <vector>

namespace {

// 截断而不是拼坏：超长标签宁可少几个字，也不能读越界
void copyInto(wchar_t* target, int capacity, const std::wstring& text) {
  if (!target || capacity <= 0) return;
  const size_t limit = static_cast<size_t>(capacity) - 1;
  const size_t n = text.size() < limit ? text.size() : limit;
  ::memcpy(target, text.c_str(), n * sizeof(wchar_t));
  target[n] = L'\0';
}

}  // namespace

extern "C" {

// 必须导出：外壳据此拒绝加载结构不兼容的旧模块
__declspec(dllexport) int AiuiShellMenuCoreVersion(void) {
  return AiUI::kShellMenuCoreApiVersion;
}

// 返回写进 items 的项数；0 表示这次选择不该出现本程序。
// 全程不抛异常：调用方是资源管理器进程，崩了就是崩资源管理器。
__declspec(dllexport) int AiuiBuildShellMenuCore(const AiUI::ShellMenuCoreSelection* selection,
                                                 int count, AiUI::ShellMenuCoreItem* items,
                                                 int capacity) {
  if (!selection || count <= 0 || !items || capacity <= 0) return 0;

  std::vector<AiUI::ShellSelectionItem> parsed;
  parsed.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    if (!selection[i].path || !*selection[i].path) continue;
    AiUI::ShellSelectionItem item;
    item.path = selection[i].path;
    item.isDirectory = selection[i].isDirectory != 0;
    parsed.push_back(std::move(item));
  }
  if (parsed.empty()) return 0;

  std::vector<AiUI::ShellMenuItem> menu;
  try {
    menu = aiuiBuildShellMenu(parsed).items;
  } catch (...) {
    return 0;
  }

  int written = 0;
  for (const auto& item : menu) {
    if (written >= capacity) break;
    AiUI::ShellMenuCoreItem& slot = items[written];
    copyInto(slot.label, AiUI::kShellMenuCoreLabelCapacity, item.label);
    copyInto(slot.arguments, AiUI::kShellMenuCoreArgsCapacity, item.arguments);
    slot.separatorAfter = item.separatorAfter ? 1 : 0;
    ++written;
  }
  return written;
}

}  // extern "C"
