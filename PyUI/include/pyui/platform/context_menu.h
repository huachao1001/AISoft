// 右键菜单：走系统原生菜单，不自绘
//
// 为什么不自绘：
//   自绘菜单要自己处理——屏幕边缘翻转、多显示器负坐标、键盘导航、无障碍、
//   高对比度主题、系统缩放、失焦消失、点菜单外关闭的鼠标捕获。这些每个平台
//   都有成熟实现，自绘只会更差，且必然与系统观感不一致。
//
// 所以这里只抽象「弹哪个菜单、选中哪一项」，菜单**模型**是共享的，
// 呈现与交互完全交给平台。
#pragma once

#include "pyui/menu.h"
#include "pyui/types.h"

#include <string>

namespace pyui {

class ContextMenuHost {
 public:
  virtual ~ContextMenuHost() = default;

  // 弹出菜单，阻塞直到用户选择或取消。
  // 返回被选中项的 id；取消返回空串。
  //
  // 后端必须保证：
  //   - 菜单自动避让屏幕边界（含多显示器负坐标）
  //   - 弹出前把 owner 提到前台，否则点别处菜单不消失
  //   - 选中后菜单关闭再返回（不要在菜单仍可见时回调）
  virtual std::string showPopupMenu(const PopupMenuRequest& request) = 0;

  // 鼠标在当前屏幕上的位置（屏幕坐标）。右键菜单用它取弹出点。
  virtual Point cursorPosition() = 0;

  // 把窗口提到前台。弹出菜单前必须调用，否则点别处菜单不消失。
  virtual void bringToFront(NativeHandle window) = 0;

  // 是否支持子菜单。不支持时后端应把子菜单项平铺或者忽略，
  // 而不是显示成不可点的父项。
  virtual bool supportsSubmenus() const { return true; }
};

}  // namespace pyui
