// `time.sleep(seconds)` —— 让当前线程睡一会儿。
//
// 与 Python 的 `time.sleep` 同义。睡的是**当前线程**：在 threading 的后台
// 任务里调用只阻塞那一个 worker，不影响主线程。
#include "aipy/pylib.h"

#include <chrono>
#include <thread>

namespace {

aipy::Value sleepImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value();
  const double seconds = args[0].toFloat();
  // 负数或 0 直接返回：Python 的 sleep 对负数抛 ValueError，而 AiPy 没有
  // 异常通道，这里选「当 0 处理」而不是让脚本崩溃。
  if (seconds <= 0.0) return aipy::Value();
  std::this_thread::sleep_for(
      std::chrono::duration<double>(seconds));
  return aipy::Value();
}

}  // namespace

AIPY_PYLIB_FUNCTION(time, sleep, sleepImpl);
