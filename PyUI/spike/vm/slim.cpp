// 验证假设：把 Value 压到 16 字节（tagged union），VM 循环能快多少。
//
// 这测的是「瘦身 Value」这一个变量的影响，其余逻辑与 vm.cpp 完全一致。
// 如果差距显著，就说明 Value 的体积是主要瓶颈，而不是分派开销。
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>

// 极简 16 字节值：只用 int / double / bool / None。
// 真实 Value 还要带 string / list / dict 指针，那也是 8 字节，
// 所以 16 字节（tag 8 + payload 8）是可达的目标。
struct SlimValue {
  enum class Type : std::uint8_t { kNone, kInt, kFloat, kBool };
  Type type = Type::kNone;
  union {
    long long int_;
    double float_;
  };

  SlimValue() = default;
  explicit SlimValue(long long v) : type(Type::kInt) { int_ = v; }
  explicit SlimValue(bool v) : type(Type::kBool) { int_ = v ? 1 : 0; }

  bool toBool() const {
    if (type == Type::kInt) return int_ != 0;
    if (type == Type::kBool) return int_ != 0;
    if (type == Type::kFloat) return float_ != 0.0;
    return false;
  }
};

static SlimValue addSlim(const SlimValue& a, const SlimValue& b) {
  // 整数路径（基准只走这条）
  if (a.type == SlimValue::Type::kInt && b.type == SlimValue::Type::kInt) {
    return SlimValue(a.int_ + b.int_);
  }
  return SlimValue(0LL);
}

static bool ltSlim(const SlimValue& a, const SlimValue& b) {
  return a.int_ < b.int_;
}

int main() {
  std::printf("sizeof(SlimValue) = %zu 字节\n", sizeof(SlimValue));

  constexpr int kIterations = 3000000;

  // total = 0; i = 0; while i < n: total += i; i += 1
  SlimValue total(0LL);
  SlimValue i(0LL);
  const SlimValue n(static_cast<long long>(kIterations));

  const auto start = std::chrono::steady_clock::now();
  while (ltSlim(i, n)) {
    total = addSlim(total, i);
    i = addSlim(i, SlimValue(1LL));
  }
  const auto finish = std::chrono::steady_clock::now();
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                          finish - start).count();

  std::printf("%d 次循环（直接函数调用）: %.2f ms\n", kIterations, micros / 1000.0);
  std::printf("  结果 %lld\n", total.int_);
  std::printf("\n注意：这是「无解释器开销」的下界。\n");
  return 0;
}
