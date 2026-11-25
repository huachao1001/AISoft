// `time.strftime(format)` / `time.localtime()` —— 日期与格式化。
//
// 与 Python 同义，但**返回列表而不是 struct_time**：AiPy 没有属性访问，
// 所以 `localtime()` 给的是 `[年, 月, 日, 时, 分, 秒, 星期]`，
// 下标 0..6 依次对应。
//
// 星期用 0=周一 … 6=周日，与 `tm_wday` 一致（Python 的 `tm_wday` 也是这个
// 口径；`strftime` 的 `%w` 才是 0=周日，两者不同，这里保留 C 的口径）。

#include "aipy/pylib.h"

#include <ctime>
#include <string>

namespace {

// 取当前本地时间。失败返回 false（几乎只会在极端时钟错误时发生）。
bool nowLocal(std::tm* out) {
  const std::time_t stamp = std::time(nullptr);
#if defined(_WIN32)
  // localtime_s 的参数顺序与 POSIX 的 localtime_r 相反，这是 Windows CRT
  // 的历史遗留；用错会静默写坏内存。
  return localtime_s(out, &stamp) == 0;
#else
  return localtime_r(&stamp, out) != nullptr;
#endif
}

aipy::Value localtimeImpl(const aipy::Value*, int) {
  std::tm parts;
  if (!nowLocal(&parts)) return aipy::Value();
  aipy::Value::List items;
  items.push_back(aipy::Value(static_cast<long long>(parts.tm_year + 1900)));
  items.push_back(aipy::Value(static_cast<long long>(parts.tm_mon + 1)));
  items.push_back(aipy::Value(static_cast<long long>(parts.tm_mday)));
  items.push_back(aipy::Value(static_cast<long long>(parts.tm_hour)));
  items.push_back(aipy::Value(static_cast<long long>(parts.tm_min)));
  items.push_back(aipy::Value(static_cast<long long>(parts.tm_sec)));
  items.push_back(aipy::Value(static_cast<long long>(parts.tm_wday)));
  return aipy::Value::list(std::move(items));
}

aipy::Value strftimeImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value(std::string());
  const std::string format = args[0].toString();
  std::tm parts;
  if (!nowLocal(&parts)) return aipy::Value(std::string());

  // 缓冲区先给 256：常见的格式串远小于它。不够时扩容重试，而不是截断——
  // 被截断的时间字符串看起来完全正常，那种错最难发现。
  std::size_t size = 256;
  for (;;) {
    std::string buffer(size, '\0');
    const std::size_t written = std::strftime(&buffer[0], size, format.c_str(), &parts);
    if (written > 0) {
      buffer.resize(written);
      return aipy::Value(buffer);
    }
    if (size > 64 * 1024) return aipy::Value(std::string());  // 格式串有毛病
    size *= 4;
  }
}

}  // namespace

AIPY_PYLIB_FUNCTION(time, localtime, localtimeImpl);
AIPY_PYLIB_FUNCTION(time, strftime, strftimeImpl);
