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
#include <memory>
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

// ── 类的语义 ──
//
// MRO、属性查找、实例化都在这里：它们是语言语义（Python 的对象模型），
// 不是 pylib 的能力，所以不能走 MethodTable 的转发机制。
//
// `lookupClassAttr` 沿 MRO 找成员；找到函数值就绑定成方法（把实例补成
// 第一个实参）。`instanceAttr` 做 attr 读的完整语义（先字段后类）。
Value classAttrOf(const Value& cls, const std::string& name);
Value instanceAttrOf(const Value& instance, const std::string& name,
                     std::string* error);
void instanceSetAttr(const Value& instance, const std::string& name,
                     const Value& value);
// 把函数值绑定到实例上，产出一个「方法值」（捕获列表头部补实例）。
Value bindMethod(const Value& function, const Value& self);
// 沿 MRO 找名字对应的函数值（不绑定）；找不到返回 None。
Value classLookupRaw(const Value& cls, const std::string& name);
// `super()` 的查找：跳过 currentClass 本身，从它的基类沿 MRO 找。
Value superLookupRaw(const Value& self, const Value& currentClass,
                     const std::string& name);

// ── 帧 ──
//
// 执行循环的私有结构，所以不在 `aipy/vm.h` 里：它不是宿主该看见的东西。
//
// `stackBase` 是这一帧在共享值栈上的起点。参数就在 [stackBase, sp) 里，
// 调用时不必拷参数，把 sp 抬高即可。

// 一个异常处理块。编译器在 `try` / `with` 前后插对应的指令，VM 在抛出时
// 弹到最近一个块。
//
// 为什么把 with 和 try 放在同一个栈上：两者都必须参与「异常向外传播」时的
// 清理，而且顺序由嵌套关系唯一决定。分成两个栈就要在展开时比较它们的进入
// 先后，那正是最容易写错的地方。
//
// `stackLevel` / `exceptionDepth` 是展开时的还原点：异常发生时值栈上可能
// 压着半截表达式，异常栈里可能有一层正在处理的内层异常，两者都要退回到
// 进入这个块那一刻的状态。
struct Block {
  enum Kind { kTry, kWith } kind = kTry;
  std::size_t target = 0;          // try: handler 入口；with: 语句之后的位置
  std::size_t stackLevel = 0;      // 展开时 sp 退回到这里
  std::size_t exceptionDepth = 0;  // 展开时 exceptions 退回到这里
  std::size_t withSlot = 0;        // with: 存管理器的局部槽
};

// 一帧的异常状态。按需分配（`Frame::exceptions` 为空表示没有）。
//
// 为什么不直接做成 Frame 的两个 vector 成员：`Frame` 是执行循环每次调用
// 都要构造的对象，白扛两个 vector 的控制块不值当，而绝大多数帧一条 try
// 都没有。
struct ExceptionState {
  std::vector<Block> blocks;
  std::vector<Value> exceptions;
};

struct Frame {
  const std::vector<std::uint8_t>* code = nullptr;
  const std::vector<LineEntry>* lines = nullptr;
  // `__init__` 帧的返回值要丢弃，改压被实例化的对象（CPython 里
  // `A()` 的结果是实例，不是 __init__ 的返回值）。非空表示这一帧是
  // 实例化调用，返回时用这个对象顶替返回值。
  Value initInstance;
  bool isInitFrame = false;
  const Function* function = nullptr;  // 模块级帧为 nullptr
  std::size_t pc = 0;
  std::size_t stackBase = 0;
  std::vector<Value> locals;
  // 处理块栈与正在处理的异常；没进过 try 的帧保持 nullptr。
  //
  // 用 unique_ptr 而不是裸指针是为了让帧被弹出时自动释放——手写 delete
  // 会漏（kReturn、异常传播、帧尾三条路径各要管一次）。
  std::unique_ptr<ExceptionState> exceptions;
};

// 取一帧的异常状态，没有就建一个。只有真的进入 try / with 时才分配：
// 多数帧一辈子用不到。
inline ExceptionState& exceptionStateOf(Frame& frame) {
  if (!frame.exceptions) frame.exceptions = std::make_unique<ExceptionState>();
  return *frame.exceptions;
}

// 只读查询：没有状态时返回 nullptr。不能和上面重载成同名——`Frame&` 的
// 实参对两者都可转换，重载会一律选中非 const 那个。
inline const ExceptionState* peekExceptionStateOf(const Frame& frame) {
  return frame.exceptions.get();
}

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
