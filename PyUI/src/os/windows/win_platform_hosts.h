// Windows 三个平台子系统的实现声明
//
// 只给 src/os/windows/ 用（AGENTS.md 铁律一：公开头文件不得出现平台类型）。
// 每个子系统各自一个 TU，但都由 platform_windows.cpp 按值持有：
// 右键菜单同时只该有一个、托盘图标只该有一个、关联登记是无状态的。
// 拆成三个文件是为了「缺哪个能力就只缺那一份实现」，而不是一个巨型 TU。
#pragma once

// 本文件的 include 顺序刻意是「先 pyui，后 windows」：windows.h 会定义
// min/max 宏、并引入大量全局符号。先让 pyui 的公开头文件看到干净的环境，
// 能避免一大类只在 Windows 后端出现的编译错误。
#include "pyui/menu.h"
#include "pyui/painter.h"
#include "pyui/platform/context_menu.h"
#include "pyui/platform/file_association.h"
#include "pyui/platform/tray.h"
#include "pyui/types.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "win_common.h"

// 托盘要 NOTIFYICONDATAW（shellapi.h），关联要 SHGetFileInfoW（shellapi.h）。
// 放在公共后端头里，省得每个 TU 各写一遍且顺序出错。
#include <shellapi.h>
#include <shlobj.h>

namespace pyui {

// ── 右键菜单 ──

class WindowsContextMenu final : public ContextMenuHost {
 public:
  std::string showPopupMenu(const PopupMenuRequest& request) override;
  Point cursorPosition() override;
  void bringToFront(NativeHandle window) override;
  bool supportsSubmenus() const override { return true; }

 private:
  // 整个菜单生命周期内只允许一个弹出：系统菜单是模态的，
  // 同时弹两个会让第二个落在第一个的所有者线程之外，行为不可预期。
  bool active_ = false;
};

// ── 文件关联 ──

class WindowsFileAssociations final : public FileAssociationHost {
 public:
  std::string registerFileAssociations(const FileAssociation& association) override;
  std::string unregisterFileAssociations(const FileAssociation& association) override;
  bool areFileAssociationsRegistered(const FileAssociation& association) override;
  InstallScope detectScope(const FileAssociation& association) override;
  Image associationIcon(const AssociationIconRequest& request) override;
  void releaseAssociationIcons() override;

 private:
  // 图标按「后缀或路径 + 像素尺寸」缓存。一个文件列表滚动时会反复问
  // 同一批后缀，不缓存就是每次去查一次 shell，滚动会明显卡。
  std::map<std::string, Image> icons_;
};

// ── 托盘 ──

class WindowsTray final : public TrayHost {
 public:
  WindowsTray();
  ~WindowsTray() override;

  bool show(const std::string& tooltip) override;
  void hide() override;
  void setTooltip(const std::string& tooltip) override;
  bool isVisible() const override { return visible_; }

  void setOnActivate(ActivateHandler fn) override { onActivate_ = std::move(fn); }
  void setMenu(MenuProvider provider, MenuHandler handler) override {
    menuProvider_ = std::move(provider);
    menuHandler_ = std::move(handler);
  }

  // 托盘窗口过程（静态，因为是窗口类回调）。由 platform_windows 的事件
  // 循环间接驱动，不需要额外线程。
  static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT processMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

 private:
  bool ensureWindow();
  void fillNotifyData(NOTIFYICONDATAW& data) const;
  void showMenu();

  HWND hwnd_ = nullptr;
  bool visible_ = false;
  std::wstring tooltip_;
  ActivateHandler onActivate_;
  MenuProvider menuProvider_;
  MenuHandler menuHandler_;
};

}  // namespace pyui
