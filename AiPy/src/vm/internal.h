// VM 的内部契约，不属于公开 API。
//
// ── 为什么单独一个头 ──
//
// `Machine::run` 是一个大 switch（约 350 行），拆开会让每条指令多一次
// 函数调用，而这一层要的正是「一次 switch + 几次直接下标」。所以执行循环
// 必须留在同一个函数里。但循环用到的三块语义——算子、方法、操作数解码——
// 与「怎么循环」是两件事，放在 `include/aipy/` 里又会变成对外承诺。
//
// 这个头只给 `src/vm/` 下的 .cpp 用。宿主 include 它是错的：里面的东西
// 随时会改，改了不该影响任何消费者。
#pragma once

#include "aipy/vm.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aipy {
namespace vm {
namespace detail {

// ── 字节码操作数 ──
//
// 定长、小端序。所有偏移都在 u16 范围内：单个函数超过 64 KB 字节码在
// 设计上就不该出现（脚本函数以几百行为量级）。
inline std::uint16_t read16(const std::vector<std::uint8_t>& code, std::size_t at) {
  return static_cast<std::uint16_t>(code[at] | (code[at + 1] << 8));
}

// ── 算子分派 ──
//
// 自由函数而不是 Machine 成员：符号名短，而且每个 Machine 实例不必各带
// 一份同样的代码。
Value applyBin(BinOp op, const Value& a, const Value& b);
Value applyCmp(CmpOp op, const Value& a, const Value& b);
Value applyUnary(UnOp op, const Value& a);

// ── 方法分派 ──
//
// `"abc".upper()`、`items.append(x)` 都编译成 kCallMethod，方法名以常量
// 下标带过来。为什么不在编译期解析成函数指针：同名方法在不同类型上行为
// 不同（`pop` 在列表上按位置、在字典上要键），而接收者类型只有运行期知道。
Value callMethod(const Value& target, const std::string& name,
                 const Value* args, int argc, std::string* error);

// ── 帧 ──
//
// 执行循环的私有结构，所以不在 `aipy/vm.h` 里：它不是宿主该看见的东西。
//
// `stackBase` 是这一帧在共享值栈上的起点。参数就在 [stackBase, sp) 里，
// 调用时不必拷参数，把 sp 抬高即可。
struct Frame {
  const std::vector<std::uint8_t>* code = nullptr;
  const std::vector<LineEntry>* lines = nullptr;
  const Function* function = nullptr;  // 模块级帧为 nullptr
  std::size_t pc = 0;
  std::size_t stackBase = 0;
  std::vector<Value> locals;
};

// ── 当前正在执行的程序 ──
//
// 扩展函数只拿到 `const Value*`，拿不到 Program。线程库要把脚本函数搬到
// 子线程去跑，就必须知道「跑哪个程序」。按线程存而不是全局变量：两个
// 线程各跑不同程序时互不影响。
const Program* currentProgramPointer();
void setCurrentProgramPointer(const Program* program);

}  // namespace detail
}  // namespace vm
}  // namespace aipy
