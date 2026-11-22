// Spike：测量 VM 的循环开销与体积。
//
// 三个数就是整个决策需要的全部依据：
//   1. 300 万次累加循环的耗时（对照 CPython 1010 ms / 转译约 3 ms）
//   2. VM 自身的 .text 增量（目标 < 100 KB）
//   3. 单次扩展调用的开销（脚本层真正高频的东西）
#include "vm.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace pyui;
using namespace pyui::vm;

namespace {

// 手写一段与下面 Python 等价的字节码：
//
//   total = 0
//   i = 0
//   while i < n:
//       total = total + i
//       i = i + 1
//   return total
//
// 局部槽：0=total 1=i 2=n
std::vector<std::uint8_t> buildLoopProgram(std::vector<Value>& consts, int* globalCount) {
  std::vector<std::uint8_t> code;
  auto put8 = [&code](std::uint8_t v) { code.push_back(v); };
  auto put16 = [&code](std::uint16_t v) {
    code.push_back(static_cast<std::uint8_t>(v & 0xFF));
    code.push_back(static_cast<std::uint8_t>(v >> 8));
  };
  auto op = [&put8](Op value) { put8(static_cast<std::uint8_t>(value)); };

  *globalCount = 1;
  consts.clear();
  consts.push_back(Value(0LL));      // 0
  consts.push_back(Value(1LL));      // 1

  // total = 0
  op(Op::kConst); put16(0);
  op(Op::kStoreLocal); put16(0);
  // i = 0
  op(Op::kConst); put16(0);
  op(Op::kStoreLocal); put16(1);
  // n = 3000000
  consts.push_back(Value(3000000LL));  // 2
  op(Op::kConst); put16(2);
  op(Op::kStoreLocal); put16(2);

  // loop_start:
  const size_t loopStart = code.size();
  // if not (i < n): break
  op(Op::kLocal); put16(1);
  op(Op::kLocal); put16(2);
  op(Op::kCmp); put8(static_cast<std::uint8_t>(CmpOp::kLt));
  op(Op::kJumpIfFalse); put16(0);  // 回填
  const size_t exitJump = code.size() - 2;

  // total = total + i
  op(Op::kLocal); put16(0);
  op(Op::kLocal); put16(1);
  op(Op::kBin); put8(static_cast<std::uint8_t>(BinOp::kAdd));
  op(Op::kStoreLocal); put16(0);

  // i = i + 1
  op(Op::kLocal); put16(1);
  op(Op::kConst); put16(1);
  op(Op::kBin); put8(static_cast<std::uint8_t>(BinOp::kAdd));
  op(Op::kStoreLocal); put16(1);

  op(Op::kJump); put16(static_cast<std::uint16_t>(loopStart));

  // exit: 回填跳转目标
  const auto exitAt = static_cast<std::uint16_t>(code.size());
  code[exitJump] = static_cast<std::uint8_t>(exitAt & 0xFF);
  code[exitJump + 1] = static_cast<std::uint8_t>(exitAt >> 8);

  op(Op::kLocal); put16(0);
  op(Op::kReturn);
  return code;
}

// 扩展调用基准：模拟「每帧里调 10 次扩展」（真实 UI 脚本的典型密度）。
Value bumpExtension(const Value* args, int count) {
  if (count < 1) return Value();
  return Value(args[0].toInt() + 1);
}

}  // namespace

int main() {
  // ── 1. 循环速度 ──
  std::vector<Value> consts;
  int globalCount = 0;
  Program program;
  program.code = buildLoopProgram(consts, &globalCount);
  program.consts = consts;
  program.globalCount = globalCount;

  Machine machine;
  const auto start = std::chrono::steady_clock::now();
  Result result = machine.run(program);
  const auto finish = std::chrono::steady_clock::now();
  const auto micros =
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();

  std::printf("300 万次循环: %.2f ms  (结果 %s)\n", micros / 1000.0,
              result.value.toString().c_str());
  std::printf("  对照 CPython: 1010 ms\n");
  std::printf("  对照 AOT 转译: ~3 ms\n");

  // ── 2. 扩展调用开销 ──
  Program extProgram;
  extProgram.consts.push_back(Value(0LL));
  extProgram.extensions.push_back(&bumpExtension);
  extProgram.globalCount = 1;
  {
    auto& code = extProgram.code;
    auto put8 = [&code](std::uint8_t v) { code.push_back(v); };
    auto put16 = [&code](std::uint16_t v) {
      code.push_back(static_cast<std::uint8_t>(v & 0xFF));
      code.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    // 100 万次：const 0 / ext bump / pop
    code.clear();
    for (int i = 0; i < 1000000; ++i) {
      put8(static_cast<std::uint8_t>(Op::kConst)); put16(0);
      put8(static_cast<std::uint8_t>(Op::kExt)); put16(0); put8(1);
      put8(static_cast<std::uint8_t>(Op::kPop));
    }
    put8(static_cast<std::uint8_t>(Op::kNone));
    put8(static_cast<std::uint8_t>(Op::kReturn));
  }
  Machine extMachine;
  const auto extStart = std::chrono::steady_clock::now();
  Result extResult = extMachine.run(extProgram);
  const auto extFinish = std::chrono::steady_clock::now();
  const auto extMicros =
      std::chrono::duration_cast<std::chrono::microseconds>(extFinish - extStart).count();
  std::printf("\n100 万次扩展调用: %.2f ms  (%.0f ns/次)\n", extMicros / 1000.0,
              extMicros * 1000.0 / 1000000.0);
  (void)extResult;

  // ── 3. 反汇编抽查（确认编码没写错）──
  std::printf("\n主程序反汇编（前 8 条）:\n");
  const auto lines = disassemble(program);
  for (size_t i = 0; i < lines.size() && i < 8; ++i) {
    std::printf("  %s\n", lines[i].c_str());
  }
  return result.failed ? 1 : 0;
}
