// Windows 右键菜单：用 TrackPopupMenuEx，不自绘
//
// 为什么不自绘：屏幕边缘翻转、多显示器负坐标、键盘导航、无障碍、高对比度、
// 系统缩放、失焦消失、点菜单外关闭的鼠标捕获——这些系统都做对了，
// 自绘只会更差，而且一定与系统观感不一致。见 DESIGN.md 4.1。
//
// 本文件只做三件事：
//   1. MenuItem 模型 → HMENU（含子菜单递归）
//   2. 确保 owner 在前台（否则点别处菜单不消失）
//   3. 把 TrackPopupMenuEx 的返回值翻译回 ItemId → id 字符串
#include "win_platform_hosts.h"

#include "pyui/menu.h"

#include "win_common.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace pyui {
namespace {

// 菜单项 id 是整数，而 MenuItem::id 是字符串（要求里明确用字符串，
// 因为菜单内容会动态变化，下标没有跨次可比性）。这里做一张表：
// 每次弹菜单重新编号，从 1 开始——0 在系统里表示「没选中」。
constexpr UINT kFirstCommandId = 1;

struct MenuBuild {
  HMENU root = nullptr;
  // id → MenuItem::id。用 vector 而不是 map：菜单项通常几十个，
  // 线性查找比哈希更快，且顺序稳定便于调试。
  std::vector<std::string> idByCommand;
  // 所有创建出来的子菜单，销毁时统一释放
  std::vector<HMENU> owned;

  ~MenuBuild() {
    for (HMENU menu : owned) {
      if (menu != nullptr) ::DestroyMenu(menu);
    }
  }
};

// 追加一项。submenu 为空表示把 title 当作一级菜单直接追加。
void appendItems(MenuBuild& build, HMENU menu, const std::vector<MenuItem>& items) {
  for (const MenuItem& item : items) {
    if (item.separator) {
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      continue;
    }

    // 标签里的 & 是助记符标记，用户文本里的 & 必须转义成 &&，
    // 否则「A & B」会显示成「A _ B」且 B 变成快捷键。
    // 制表符之后是快捷键说明（如「复制\tCtrl+C」），系统菜单靠 \t
    // 把它推到右列；这里原样保留。
    std::wstring label = win32::toWide(item.label);
    {
      std::wstring escaped;
      escaped.reserve(label.size() + 4);
      for (wchar_t c : label) {
        escaped.push_back(c);
        if (c == L'&') escaped.push_back(L'&');
      }
      label.swap(escaped);
    }

    if (!item.children.empty()) {
      HMENU submenu = ::CreatePopupMenu();
      if (submenu == nullptr) continue;
      build.owned.push_back(submenu);
      appendItems(build, submenu, item.children);
      ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), label.c_str());
      continue;
    }

    const UINT command = static_cast<UINT>(build.idByCommand.size()) + kFirstCommandId;
    build.idByCommand.push_back(item.id);

    UINT flags = MF_STRING;
    if (!item.enabled) flags |= MF_GRAYED;
    if (item.checked) flags |= MF_CHECKED;
    if (item.topLevel) flags |= MF_DEFAULT;
    ::AppendMenuW(menu, flags, command, label.c_str());
  }
}

}  // namespace

std::string WindowsContextMenu::showPopupMenu(const PopupMenuRequest& request) {
  if (request.items.empty()) return std::string();
  // 系统菜单是模态的：重入会得到一个「挂在不存在所有者上的菜单」，
  // 表现为点别处不消失、甚至把整个 UI 线程卡死。
  if (active_) return std::string();

  MenuBuild build;
  build.root = ::CreatePopupMenu();
  if (build.root == nullptr) return std::string();
  build.owned.push_back(build.root);

  appendItems(build, build.root, request.items);
  if (build.idByCommand.empty()) {
    // 全是分隔线或全是父项：没有可选项，不要弹出一个空菜单。
    return std::string();
  }

  auto* owner = static_cast<HWND>(request.owner);
  if (owner == nullptr) owner = ::GetActiveWindow();

  // 强制前台：不做的话，点菜单外面菜单不会消失，用户必须再点一次窗口。
  // 这是 Win32 弹出菜单最经典的坑，SetForegroundWindow 就是它的解药。
  if (owner != nullptr) {
    ::SetForegroundWindow(owner);
    ::BringWindowToTop(owner);
  }

  active_ = true;
  // TPM_RETURNCMD：让 TrackPopupMenuEx 直接返回命令号，而不是发 WM_COMMAND。
  // 用返回值比收 WM_COMMAND 简单得多——不需要一张「当前菜单 id」的全局状态，
  // 也不怕菜单项和别的命令号撞车。
  //
  // 不传 TPM_LEFTALIGN/TPM_TOPALIGN：让系统按「屏幕边缘自动翻转」决定
  // 往哪边展开。这两个 flag 会强制往右下展开，靠近屏幕右缘时菜单就被裁掉了。
  const UINT command = ::TrackPopupMenuEx(build.root, TPM_RETURNCMD | TPM_LEFTBUTTON,
                                          request.screenPosition.x, request.screenPosition.y,
                                          owner, nullptr);
  active_ = false;

  // 菜单关闭后系统可能把前台还回去；再补一次，窗口的键盘焦点不会丢。
  if (owner != nullptr) ::PostMessageW(owner, WM_NULL, 0, 0);

  if (command < kFirstCommandId) return std::string();
  const size_t index = command - kFirstCommandId;
  if (index >= build.idByCommand.size()) return std::string();
  return build.idByCommand[index];
}

Point WindowsContextMenu::cursorPosition() {
  POINT cursor{};
  if (!::GetCursorPos(&cursor)) return Point{};
  return Point{cursor.x, cursor.y};
}

void WindowsContextMenu::bringToFront(NativeHandle window) {
  auto* hwnd = static_cast<HWND>(window);
  if (hwnd == nullptr) return;
  // 最小化的窗口不能直接 SetForegroundWindow（它还在任务栏里躺着），
  // 先还原再提前台。
  if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);
  ::SetForegroundWindow(hwnd);
  ::BringWindowToTop(hwnd);
}

}  // namespace pyui
