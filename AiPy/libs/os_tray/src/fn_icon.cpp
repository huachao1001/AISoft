// `tray.icon(path, tooltip)` —— 设置托盘图标与悬停提示。
//
// 两个参数都可省：`tray.icon()` 用系统默认图标、空提示。图标路径必须在
// `tray.run()` 之前给（图标是 run() 创建窗口时加载的），之后再调只改状态、
// 不换已显示的图标。
//
// 为什么要 .ico 而不是 .png：Windows 的 `LoadImage(..., IMAGE_ICON)` 只认
// 图标资源或 .ico 文件。png 要自己解码再建 HICON，那是另一套代码。
//
// 失败不影响后续：路径不存在时 run() 用系统默认图标，脚本仍然能跑——托盘
// 程序「图标不好看」比「整个程序起不来」好排查得多。
#include "aipy/pylib.h"
#include "aipy/os_tray/tray.h"

#include <string>

namespace {

aipy::Value iconImpl(const aipy::Value* args, int count) {
  aipy::os_tray::State& state = aipy::os_tray::state();
  if (count >= 1) state.iconPath = args[0].toString();
  if (count >= 2) state.tooltip = args[1].toString();
  // 没有「设置失败」这个信息：文件在 run() 时才打开，此刻不知道能不能读。
  return aipy::Value(true);
}

}  // namespace

AIPY_PYLIB_FUNCTION_NAMED(os_tray, "os.tray", icon, iconImpl);
