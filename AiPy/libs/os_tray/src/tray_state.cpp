// 托盘状态的存放处。
//
// ── 为什么单独一个编译单元 ──
//
// `tray.icon` / `tray.item` / `tray.on` 各在一个 .cpp 里，它们都要读同一份
// 状态；`tray.run` 也要。状态放在这里，那些 .cpp 各自只引用符号、不复制
// 定义——按需链接仍然按函数挑 obj。
//
// 为什么是进程级单例而不是 kObject 句柄：Windows 一个进程的托盘图标通常只
// 挂在一个窗口上（脚本模型里没有「哪一个图标」的概念），多图标会让点击
// 回调分不清是谁点的。要多个就起多个进程。
#include "aipy/os_tray/tray.h"

namespace aipy {
namespace os_tray {

State& state() {
  static State instance;
  return instance;
}

bool findHandler(const std::string& id, Value* out) {
  const State& current = state();
  for (const State::Handler& handler : current.handlers) {
    if (handler.id != id) continue;
    if (out != nullptr) *out = handler.fn;
    return true;
  }
  return false;
}

}  // namespace os_tray
}  // namespace aipy
