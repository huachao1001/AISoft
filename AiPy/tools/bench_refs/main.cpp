// 基准：原子引用计数相对非原子的代价。
//
// 测三类操作，对应脚本里最常见的三种负载：
//   1. 容器句柄拷贝   —— `b = a`（list/dict 共享），最密集地碰 refs
//   2. 算术求值       —— `i += 1`，VM 主循环的形态，几乎不碰 refs
//   3. 字符串构造     —— 每次新建 HeapData，只碰一次 refs
//
// 用法: bench_refs.exe   输出三类操作的毫秒数。
#include "aipy/value.h"

#include <chrono>
#include <cstdio>
#include <string>

using aipy::Value;

namespace {

double ms(std::chrono::steady_clock::duration span) {
  return std::chrono::duration<double, std::milli>(span).count();
}

}  // namespace

int main() {
  // ── 1. 容器句柄拷贝：500 万次 `b = a` ──
  const int copies = 5000000;
  Value list = Value::list({Value(1LL), Value(2LL), Value(3LL)});
  auto t0 = std::chrono::steady_clock::now();
  long long sink = 0;
  for (int i = 0; i < copies; ++i) {
    Value copy = list;
    sink += static_cast<long long>(copy.asList().size());
  }
  auto t1 = std::chrono::steady_clock::now();

  // ── 2. 算术求值：300 万次，模拟 VM 的 `i += 1` ──
  const int steps = 3000000;
  Value acc(0LL);
  Value one(1LL);
  auto t2 = std::chrono::steady_clock::now();
  for (int i = 0; i < steps; ++i) {
    acc = aipy::add(acc, one);
  }
  auto t3 = std::chrono::steady_clock::now();

  // ── 3. 字符串构造：100 万次新建 HeapData ──
  const int builds = 1000000;
  auto t4 = std::chrono::steady_clock::now();
  long long total = 0;
  for (int i = 0; i < builds; ++i) {
    Value text(std::string("item-") + std::to_string(i));
    total += static_cast<long long>(text.asString().size());
  }
  auto t5 = std::chrono::steady_clock::now();

  std::printf("容器句柄拷贝  %2d00万次  %7.1f ms\n", copies / 1000000, ms(t1 - t0));
  std::printf("算术求值      %2d00万次  %7.1f ms\n", steps / 1000000, ms(t3 - t2));
  std::printf("字符串构造    %2d00万次  %7.1f ms\n", builds / 1000000, ms(t5 - t4));
  std::printf("(校验 %lld %lld %lld)\n", sink, acc.toInt(), total);
  return 0;
}
