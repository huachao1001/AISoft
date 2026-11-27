// `tray.run()` —— 进入消息循环，直到 `tray.quit()` 或窗口被关掉。
//
// 这个函数**阻塞**到程序该退出为止。它是脚本的最后一句：之后的一切都由
// 菜单项与左键回调驱动。
//
// 单独一个编译单元：不调 run() 的脚本（比如只弹一次气泡提示）不该把窗口
// 类注册与消息泵拖进产物。
#include "aipy/pylib.h"
#include "aipy/os_tray/tray.h"

namespace {

aipy::Value runImpl(const aipy::Value*, int) {
  return aipy::Value(aipy::os_tray::run());
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_tray, "os.tray", run, runImpl);
