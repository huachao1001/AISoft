// 分离两个变量：解释器分派开销 vs Value 运算开销。
//
// 上一轮把 805 ms 全归给「拷贝 96 字节的 Value」，但瘦到 16 字节后只降到
// 477 ms。说明判断错了，必须实测拆开，否则后面所有优化都建立在错的前提上。
//
// 三个探针：
//   A. 纯分派：同样的字节码，但算术换成「直接读写 int 数组」
//   B. 真实 VM：走 py::add
//   C. 直接调用 py::add：不经解释器
#include "pyui/py_value.h"

#include <chrono>
#include <cstdio>
#include <vector>

using pyui::py::Value;

constexpr int kIterations = 3000000;

// A. 纯分派：一个最小的 switch 解释器，载荷是 8 字节整数。
//    它测的是「取指 + 跳转 + 维护栈」的固定成本，与 Value 无关。
namespace {

enum class Op : unsigned char { kConst, kLocal, kStoreLocal, kAdd, kLt, kJumpIfFalse, kJump, kReturn };

struct Tiny {
  std::vector<unsigned char> code;
  std::vector<long long> consts;
  long long locals[4] = {0, 0, 0, 0};
  long long stack[8] = {0};
  int sp = 0;
  long long run() {
    size_t pc = 0;
    auto u16 = [this](size_t at) {
      return static_cast<int>(code[at] | (code[at + 1] << 8));
    };
    while (pc < code.size()) {
      switch (static_cast<Op>(code[pc++])) {
        case Op::kConst: stack[sp++] = consts[u16(pc)]; pc += 2; break;
        case Op::kLocal: stack[sp++] = locals[u16(pc)]; pc += 2; break;
        case Op::kStoreLocal: locals[u16(pc)] = stack[--sp]; pc += 2; break;
        case Op::kAdd: --sp; stack[sp - 1] += stack[sp]; break;
        case Op::kLt: --sp; stack[sp - 1] = stack[sp - 1] < stack[sp]; break;
        case Op::kJumpIfFalse: {
          const bool truthy = stack[--sp] != 0;
          if (truthy) {
            pc += 2;  // 条件成立：跳过跳转目标
          } else {
            pc = static_cast<size_t>(u16(pc));
          }
          break;
        }
        case Op::kJump: pc = static_cast<size_t>(u16(pc)); break;
        case Op::kReturn: return stack[--sp];
      }
    }
    return 0;
  }
};

std::vector<unsigned char> buildTiny() {
  std::vector<unsigned char> c;
  auto put = [&c](unsigned char v) { c.push_back(v); };
  auto op = [&put](Op o) { put(static_cast<unsigned char>(o)); };
  auto u16 = [&put](int v) { put(static_cast<unsigned char>(v & 0xFF)); put(static_cast<unsigned char>(v >> 8)); };

  op(Op::kConst); u16(0); op(Op::kStoreLocal); u16(0);   // total = 0
  op(Op::kConst); u16(0); op(Op::kStoreLocal); u16(1);   // i = 0
  op(Op::kConst); u16(1); op(Op::kStoreLocal); u16(2);   // n = 3000000
  const size_t loop = c.size();
  op(Op::kLocal); u16(1); op(Op::kLocal); u16(2); op(Op::kLt);
  op(Op::kJumpIfFalse); u16(0);
  const size_t patch = c.size() - 2;
  op(Op::kLocal); u16(0); op(Op::kLocal); u16(1); op(Op::kAdd); op(Op::kStoreLocal); u16(0);
  op(Op::kLocal); u16(1); op(Op::kConst); u16(0); op(Op::kAdd); op(Op::kStoreLocal); u16(1);
  op(Op::kJump); u16(static_cast<int>(loop));
  const auto exit = static_cast<int>(c.size());
  c[patch] = static_cast<unsigned char>(exit & 0xFF);
  c[patch + 1] = static_cast<unsigned char>(exit >> 8);
  op(Op::kLocal); u16(0); op(Op::kReturn);
  return c;
}

template <typename Fn>
double timeMs(Fn&& fn) {
  const auto start = std::chrono::steady_clock::now();
  fn();
  const auto finish = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count() / 1000.0;
}

}  // namespace

int main() {
  std::printf("sizeof(Value) = %zu 字节\n\n", sizeof(Value));

  // A. 纯分派
  Tiny tiny;
  tiny.code = buildTiny();
  tiny.consts = {1, static_cast<long long>(kIterations)};
  long long tinyResult = 0;
  const double a = timeMs([&] { tinyResult = tiny.run(); });
  std::printf("A. 纯分派（long long 载荷）: %8.2f ms  结果 %lld\n", a, tinyResult);

  // C. 直接调用 py::add（不经解释器）：同样的循环，C++ 里手写
  Value directTotal(0LL);
  Value directI(0LL);
  const Value step(1LL);
  const Value limit(static_cast<long long>(kIterations));
  const double c = timeMs([&] {
    while (pyui::py::lt(directI, limit).toBool()) {
      directTotal = pyui::py::add(directTotal, directI);
      directI = pyui::py::add(directI, step);
    }
  });
  std::printf("C. 直调 py::add（不经解释器）: %8.2f ms  结果 %s\n", c,
              directTotal.toString().c_str());

  std::printf("\n差额 = 解释器分派成本（B-A），B 见 bench.exe\n");
  return 0;
}
