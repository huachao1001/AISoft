// `tray.on(id, fn)` —— 把脚本函数绑到菜单项 / 左键点击上。
//
// 第一个参数是菜单项的 id（与 `tray.item` 给的相同）；保留 id `"click"`
// 表示左键点击。第二个参数是**函数值**——`def` 出来的名字、lambda、绑定
// 方法都行。
//
// ── 为什么传值而不是名字 ──
//
// AiPy 有一等函数，`tray.on("quit", quit_app)` 里的 `quit_app` 读出来就是
// 函数值。传名字的话库层要再写一遍「按名字查 Program::functions」，而且
// 拿不到 lambda 与闭包（它们没有名字）。
//
// ── 回调什么时候执行 ──
//
// 在 `tray.run()` 的消息循环里、**调用 run 的那个线程**上同步执行。所以
// 回调里读写模块级变量不需要加锁。
#include "aipy/pylib.h"
#include "aipy/os_tray/tray.h"

#include <string>

namespace {

aipy::Value onImpl(const aipy::Value* args, int count) {
  if (count < 2) return aipy::Value(false);
  const std::string id = args[0].toString();
  if (id.empty()) return aipy::Value(false);

  // 不是函数值就拒绝：给一个字符串或数字会在点击时静默什么也不发生，
  // 而那是最难查的一类「配置写错了」。
  const aipy::Value& fn = args[1];
  if (!aipy::isFunction(fn) && !aipy::isBuiltinFunction(fn)) {
    aipy::diagnose("tray.on(" + id + "): the second argument is not a function");
    return aipy::Value(false);
  }

  aipy::os_tray::State& state = aipy::os_tray::state();
  if (id == aipy::os_tray::kClickId) state.hasClickHandler = true;

  for (aipy::os_tray::State::Handler& existing : state.handlers) {
    if (existing.id != id) continue;
    existing.fn = fn;  // 重复注册时换实现，不追加
    return aipy::Value(true);
  }
  state.handlers.push_back(aipy::os_tray::State::Handler{id, fn});
  return aipy::Value(true);
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_tray, "os.tray", on, onImpl);
