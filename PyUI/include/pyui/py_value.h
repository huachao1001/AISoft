// 动态值：实现已迁到 AiPy（`aipy/value.h`）。
//
// ── 为什么这里只剩转发 ──
//
// `Value` 与它的全部内建是「Python 语义」，不是「界面能力」。它们必须能
// 在没有窗口的环境里工作（命令行、测试、将来的服务端宿主），所以实现
// 归 AiPy；PyUI 只是**使用**它。
//
// 这里保留 `pyui::py` 这个名字不是历史包袱：生成的代码、50+ 处界面调用、
// `py_script.h` 的整套脚本 API 都用 `py::` 前缀。改名会让几十个文件
// 同时变动，而收益只是「名字更好看」——不划算。用 using 把 AiPy 的名字
// 引入进来，既保住了调用点，也让「谁拥有实现」一目了然。
#pragma once

#include "aipy/value.h"

namespace pyui {
namespace py {

using namespace aipy;

// `List` 是唯一需要显式带进来的类型别名：`using namespace` 会把名字
// 引入作用域，但类内 `using List = ...` 这种声明在嵌套类里查不到。
using aipy::List;

}  // namespace py
}  // namespace pyui
