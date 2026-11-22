// Value 的尺寸与拷贝成本。VM 每次 push/pop 都拷贝它，所以这两项直接决定
// 解释器性能——96 字节时 300 万次循环 805 ms，这是瘦身的出发点。
#include "pyui/py_value.h"

#include <chrono>
#include <cstdio>

using pyui::py::Value;

// 放在命名空间作用域：lambda 里用它是常量表达式，不必捕获。
constexpr int kCount = 10000000;

int main() {
  std::printf("sizeof(Value)       = %zu 字节\n", sizeof(Value));
  std::printf("sizeof(std::string) = %zu 字节\n", sizeof(std::string));

  // 三个载荷类型各拷 kCount 次，看引用计数带来的额外成本。
  const Value intValue(12345LL);
  const Value textValue("hello");
  const auto listValue = Value::list({Value(1LL), Value(2LL)});

  auto bench = [](const char* label, const Value& source) {
    std::int64_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kCount; ++i) {
      const Value copy = source;
      sum += copy.isNumber() ? copy.toInt() : 1;
    }
    const auto finish = std::chrono::steady_clock::now();
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();
    std::printf("  %-8s 拷贝 %.2f ms (%.1f ns/次)\n", label, micros / 1000.0,
                micros * 1000.0 / kCount);
    return sum;
  };

  std::int64_t sink = 0;
  std::printf("拷贝 %d 次:\n", kCount);
  sink += bench("int", intValue);
  sink += bench("string", textValue);
  sink += bench("list", listValue);
  std::printf("(sink=%lld)\n", static_cast<long long>(sink));
  return 0;
}
