// 系统外壳集成：文件管理器右键菜单与托盘图标
//
// 应用只说"要挂什么动作、托盘上放什么菜单"，落到注册表、LaunchAgent
// 还是 .desktop 由平台后端决定，应用侧不接触任何平台类型。
#pragma once

#include "AiUI/menu.h"
#include "AiUI/shell_menu.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AiUI {

class PlatformTray;

// ── 文件管理器右键菜单 ──

// 一个动作。应用只声明"对哪些对象生效、叫什么、带什么参数启动自己"。
//   ShellVerb compress;
//   compress.id = L"AIZip.Compress";
//   compress.label = L"压缩该文件...";
//   compress.command = L"-compress";
//   registerShellVerbs({compress});
struct ShellVerb {
  // 稳定标识，如 "AIZip.Extract"。同一程序内必须唯一，也是排序依据。
  std::wstring id;
  // 菜单显示文字
  std::wstring label;
  // 追加在自身可执行文件后的参数，如 "-extract"；目标路径由后端按
  // 平台约定拼在后面，应用不用管。
  std::wstring command;
  // 生效对象：文件后缀（".zip"）、"*"（所有文件）、"Directory"（所有目录）。
  // 留空表示所有文件与目录。
  std::vector<std::wstring> targets;
  // 从生效对象里排除的后缀，如 {".zip", ".7z"}。只对标了 "*" 的动词有意义：
  // 通配符本身没法"减去某几类"，后端得把它翻译成自己的条件表达式。
  std::vector<std::wstring> excludeTargets;
  // 置顶到菜单最前；false 时按 id 排序，同前缀的动作自然相邻
  bool topLevel = false;
  // 只在单选时出现。多选时"解压到某处"没有明确语义，显示出来只会误导。
  bool singleSelectionOnly = false;
};

// 挂上动作；成功返回空串，失败返回可读原因
std::wstring registerShellVerbs(const std::vector<ShellVerb>& verbs);
std::wstring unregisterShellVerbs(const std::vector<ShellVerb>& verbs);
// 是否已挂好且仍指向当前可执行文件
bool areShellVerbsRegistered(const std::vector<ShellVerb>& verbs);

// ── 动态菜单处理器 ──
//
// 静态动词的菜单文字在注册那一刻就定死了，做不出「压缩到 rnd.zip」这种按
// 文件名算出来的文案。动态文案只能由文件管理器进程内的处理器在弹出菜单的
// 瞬间计算，所以这里把处理器也抽象出来：
//
//   - 组件本体（进程内 COM 组件等）由 cmake 的 aiui_add_shell_menu_dll 构建，
//     平台侧实现留在 src/os/<platform>/，应用不写任何系统代码；
//   - 应用只实现 aiuiBuildShellMenu（见 AiUI/shell_menu.h）：输入选中项，
//     输出菜单项。
struct ShellMenuHandler {
  // 稳定标识，如 "AIZip"。同一程序内唯一；系统里的注册位置与组件标识都由它推出，
  // 所以发布后不能再改，否则已安装用户的菜单会失效。
  std::wstring id;
  // 组件的类标识文本（"{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"）。
  // 由构建脚本按 id 派生，应用与组件各拿一份，必须一致。
  std::wstring clsid;
  // 组件释放到用户目录后的文件名，如 "aicmd.dll"，也是平台约定的组件形态。
  std::wstring componentName;
  // 组件在本可执行文件 .rc 里的资源 id（RCDATA），由后端取出后落盘。
  int componentResourceId = 200;
  // 系统诊断工具里显示的组件名，只用于可读性
  std::wstring displayName;
  // 生效对象，语义同 ShellVerb::targets：{"*"} 所有文件、{"Directory"} 所有目录。
  std::vector<std::wstring> targets;
};

// 释放组件并注册；成功返回空串，失败返回可读原因
std::wstring registerShellMenuHandler(const ShellMenuHandler& handler);
// 撤销注册。已落盘的组件文件保留，避免占用（文件管理器可能仍加载着它）。
std::wstring unregisterShellMenuHandler(const ShellMenuHandler& handler);
bool isShellMenuHandlerRegistered(const ShellMenuHandler& handler);

// ── 托盘图标 ──

// 图标由后端从当前可执行文件取，应用只给提示文字与菜单，避免再引一套
// 跨平台的图片约定。首次 show() 时自动创建平台侧宿主。
class TrayIcon {
 public:
  TrayIcon();
  ~TrayIcon();

  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;

  // 显示图标；tooltip 是悬停提示。重复调用只更新内容。
  // 平台不支持托盘时静默失败，用 isVisible() 判断。
  void show(const std::wstring& tooltip);
  void hide();
  bool isVisible() const { return visible_; }
  // 图标已显示时更新悬停提示
  void setTooltip(const std::wstring& tooltip);

  // 左键单击（或双击，看平台习惯）时回调；通常用来还原主窗口
  using ActivateHandler = std::function<void()>;
  void setOnActivate(ActivateHandler fn) { onActivate_ = std::move(fn); }

  // 右键菜单内容与选中回调；index 是 provider 返回列表里的下标
  using MenuProvider = std::function<std::vector<MenuItem>()>;
  using MenuHandler = std::function<void(int index)>;
  void setMenu(MenuProvider provider, MenuHandler handler) {
    menuProvider_ = std::move(provider);
    menuHandler_ = std::move(handler);
  }

 private:
  // 平台侧回调的接收者；定义在 shell.cpp，能访问外层的私有成员
  class Host;

  bool ensureTray();

  std::unique_ptr<Host> host_;
  std::unique_ptr<PlatformTray> tray_;
  bool visible_ = false;
  ActivateHandler onActivate_;
  MenuProvider menuProvider_;
  MenuHandler menuHandler_;
};

}  // namespace AiUI
