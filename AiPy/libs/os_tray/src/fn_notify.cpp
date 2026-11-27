// `tray.notify(title, text)` —— 弹一个气泡提示。
//
// 只对已挂上的图标有效：气泡是 `Shell_NotifyIcon(NIM_MODIFY)` 的一个字段，
// 没有图标就没有地方显示。所以它只能在 `tray.run()` 的回调里调，或者由
// 另起的线程在图标存在期间调。
//
// 失败返回 False：没有图标、系统不允许（比如专注助手开着）都会失败。
// 脚本把「提示没弹出来」当成可接受的情况即可，不该因为它中断流程。
#include "aipy/pylib.h"
#include "aipy/os_tray/tray.h"

#include <string>

namespace {

aipy::Value notifyImpl(const aipy::Value* args, int count) {
  const std::string title = count >= 1 ? args[0].toString() : std::string();
  const std::string text = count >= 2 ? args[1].toString() : std::string();
  return aipy::Value(aipy::os_tray::notify(title, text));
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_tray, "os.tray", notify, notifyImpl);
