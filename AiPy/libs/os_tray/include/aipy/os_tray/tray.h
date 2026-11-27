// `os.tray` pylib 的对外契约。
//
// ── 它是什么 ──
//
// 一个托盘图标 + 右键菜单 + 左键回调。脚本写：
//
//   import os.tray
//   os.tray.icon("app.ico", "我的程序")
//   os.tray.item("open", "打开")
//   os.tray.separator()
//   os.tray.item("quit", "退出")
//   os.tray.on("open", open_main)
//   os.tray.on("click", open_main)
//   os.tray.on("quit", quit_app)
//   os.tray.run()
//
// ── 为什么必须有一个阻塞的 run() ──
//
// 托盘图标依附在一个窗口上：窗口的消息循环一停，图标就不再响应（点了没
// 反应，系统把图标标成「无响应」）。所以托盘程序的主线程必须停在消息循环
// 里。这与「脚本顺序执行」不矛盾——`os.tray.run()` 就是最后一句，之后的一切
// 都由回调驱动。
//
// ── 回调怎么回到脚本 ──
//
// `os.tray.on(id, fn)` 收的是**函数值**（`Value` 里的 kObject 函数），回调
// 发生时 `Machine::callValue` 建独立帧跑它——与 `threading` 用的是同一套
// 「独立帧栈」机制。
//
// 为什么不用「传函数名」：AiPy 已有一等函数（`f = len`、lambda），而托盘
// 回调天然是「注册一个东西、之后被调用」，用名字反而要再写一遍查表，且
// 拿不到闭包。
//
// ── 线程边界 ──
//
// 消息循环在**调用 `os.tray.run()` 的那个线程**上跑（通常就是主线程）。回调
// 也在那个线程里同步执行，所以回调里读写模块级变量不需要加锁——与
// CPython 的主线程模型一致。要在后台干活请用 `threading`。
//
// ── 平台 ──
//
// 只有 Windows。别的平台上 `os.tray.run()` 报诊断并返回 False，而不是静默
// 什么都不做。
#pragma once

#include "aipy/vm.h"

#include <string>
#include <vector>

namespace aipy {
namespace os_tray {

// 一条菜单项。`id == "-"` 表示分隔线。
struct MenuItem {
  std::string id;
  std::string label;
};

// 分隔线在菜单项表里的标记 id。
inline constexpr const char* kSeparatorId = "-";

// 左键回调在回调表里的保留 id。脚本用它注册，也只有一个槽。
inline constexpr const char* kClickId = "click";

// 托盘状态：图标、提示、菜单项、回调表。
//
// 进程级单例：Windows 的一个进程通常只有一个托盘图标（同一个 HWND 能挂
// 多个，但脚本模型里没有「哪一个」的概念，多图标只会让点击回调分不清是谁
// 点的）。要多个就起多个进程。
struct State {
  // 图标文件路径（.ico）。空表示用系统默认。
  std::string iconPath;
  // 鼠标悬停时的提示文本。
  std::string tooltip;

  // 菜单项，按加入顺序显示。
  std::vector<MenuItem> items;

  // id → 回调函数值。存 Value 而不是名字：回调是脚本传进来的函数值，
  // 名字反而要再查一次表，且 lambda 没有名字。
  struct Handler {
    std::string id;
    Value fn;
  };
  std::vector<Handler> handlers;

  // 有没有注册左键回调。
  bool hasClickHandler = false;
};

// 进程级状态。定义在 tray_state.cpp。
State& state();

// 找 id 对应的回调；没有返回 false。
bool findHandler(const std::string& id, Value* out);

// 跑消息循环，直到 quit 被调用或窗口被关闭。返回 false 表示创建失败。
//
// 定义在 tray_loop.cpp：不调 run() 的脚本不该把窗口类注册与消息泵拖进
// 产物。
bool run();

// 请求退出消息循环（供 `tray.quit()` 与脚本自己用）。
void quit();

// 弹一个气泡提示。需要先跑 `os.tray.run()`（气泡挂在那个图标上），所以它与
// run 放在同一个编译单元里。
bool notify(const std::string& title, const std::string& text);

}  // namespace os_tray
}  // namespace aipy
