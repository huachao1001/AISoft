// `time.monotonic()` —— 单调递增的秒数，只用于算间隔。
//
// 与 Python 的 `time.monotonic()` 同义。为什么单独存在：`time.time()` 在
// 系统改时间（校时、夏令时）时会跳，用两次 time() 相减算耗时会得到负数或
// 离谱的值。monotonic 永远往前走。
//
// 起点是任意的（不保证与纪元有关），只能相减、不能当日期用——这正是与
// `time.time()` 分开的理由。
#include "aipy/pylib.h"

#include <chrono>

namespace {

aipy::Value monotonicImpl(const aipy::Value*, int) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const double seconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 1e9;
  return aipy::Value(seconds);
}

}  // namespace

AIPY_PYLIB_FUNCTION(time, monotonic, monotonicImpl);
