// `threading.current_thread()` —— 当前线程的名字。
//
// ── 为什么返回字符串而不是 Thread 对象 ──
//
// CPython 返回 Thread 对象、脚本写 `current_thread().name`。AiPy 没有属性
// 访问（只有方法调用），而这个信息本来就只是一个名字，返回字符串更直接：
// `print(threading.current_thread())` 就是想要的输出。
//
// 主线程叫 "MainThread"（与 CPython 一致），池里的工作线程叫 "Worker-N"。
#include "aipy/threading/pool.h"
#include "aipy/pylib.h"

namespace aipy {
namespace threading {
namespace {

Value currentThreadImpl(const Value*, int) {
  return Value(currentThreadName());
}

}  // namespace

AIPY_PYLIB_FUNCTION(threading, current_thread, currentThreadImpl);

}  // namespace threading
}  // namespace aipy
