// `tray.quit()` —— 请求退出消息循环。
//
// 从菜单回调里调它是退出程序的标准做法。也可以在脚本别处调（比如收到某个
// 外部信号），效果相同。
#include "aipy/pylib.h"
#include "aipy/os_tray/tray.h"

namespace {

aipy::Value quitImpl(const aipy::Value*, int) {
  aipy::os_tray::quit();
  return aipy::Value();
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_tray, "os.tray", quit, quitImpl);
