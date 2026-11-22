// 轻量字节码 VM —— 原型（spike）
//
// 目标：核心只提供最基础的 Python 语义，其余能力全部由扩展提供，
// import 是唯一入口。这个 VM 就是「基础语义」的执行器。
//
// 三条硬约束，决定了这里的每一个设计选择：
//   1. **体积**：VM 自身的 .text 必须很小（目标 < 100 KB）。所以不用
//      unordered_map、不用 std::function、不在热路径上分配。
//   2. **可预测**：指令是定长操作数 + 单字节操作码，分派就是一个 switch。
//      没有 JIT、没有内联缓存、没有隐藏的间接跳转。
//   3. **可扩展**：扩展函数是裸函数指针（Value(*)(const Value*, int)），
//      这样 DLL 与静态库能用同一个签名，调用边界上不分配容器。
#pragma once

#include "pyui/py_value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pyui {
namespace vm {

// Value 定义在 pyui::py（脚本层的动态值）。VM 是它的执行器，
// 所以这里只引入名字，不复用 py 命名空间——VM 与「脚本门面」是两层。
using Value = py::Value;

// 扩展函数签名。故意用「指针 + 个数」而不是 std::vector：
// 调用边界上不分配、不拷贝容器，DLL 扩展也能直接实现它。
using ExtensionFn = Value (*)(const Value* args, int count);

// 操作码。单字节，按使用频率排列（热的那几个落在前半段，
// 对分支预测略友好；更重要的是让 0 号操作码是最高频的常量加载）。
enum class Op : std::uint8_t {
  kConst = 0,        // u16 常量下标
  kLocal,            // u16 局部槽
  kStoreLocal,       // u16 局部槽
  kGlobal,           // u16 全局槽
  kStoreGlobal,      // u16 全局槽
  kBin,              // u8 二元算子
  kCmp,              // u8 比较算子
  kJumpIfFalse,      // i16，弹出条件
  kJump,             // i16
  kReturn,
  kCall,             // u16 函数号, u8 实参个数
  kExt,              // u16 扩展函数号, u8 实参个数
  kNone,
  kTrue,
  kFalse,
  kUnary,            // u8 一元算子
  kPop,
  kDup,
  kList,             // u16 元素个数
  kDict,             // u16 键值对个数
  kGetItem,
  kSetItem,
  // and/or 要「返回操作数本身」，所以不能先弹出再判断：
  // 条件为真/假时把栈顶留在原位，直接跳走。
  kJumpIfFalseKeep,  // i16，条件为假时保留栈顶（and）
  kJumpIfTrueKeep,   // i16，条件为真时保留栈顶（or）
};

enum class BinOp : std::uint8_t {
  kAdd, kSub, kMul, kDiv, kFloorDiv, kMod, kPow,
  kBitAnd, kBitOr, kBitXor, kShl, kShr,
};

enum class CmpOp : std::uint8_t {
  kEq, kNe, kLt, kLe, kGt, kGe, kIn, kNotIn,
};

enum class UnOp : std::uint8_t { kNeg, kNot, kInvert };

// 一个函数（含脚本里的 def）。
struct Function {
  std::vector<std::uint8_t> code;
  int paramCount = 0;
  int localCount = 0;
};

// 一个编译好的程序：主代码 + 函数表 + 常量池 + 扩展表。
struct Program {
  std::vector<std::uint8_t> code;       // 模块级代码
  std::vector<Function> functions;
  std::vector<Value> consts;
  std::vector<ExtensionFn> extensions;  // 编译期解析好的函数指针
  std::vector<std::string> extensionNames;  // 仅用于报错信息
  int globalCount = 0;
};

// 执行结果：值 + 是否出错。
//
// 不用异常：异常会让每个调用点都多一份 unwind 表，体积和速度都吃亏，
// 而脚本层本来就没有 try/except（见 docs/SUPPORT.md 的 C 类）。
struct Result {
  Value value;
  bool failed = false;
  std::string error;  // 空表示成功
};

class Machine {
 public:
  Machine() = default;

  // 执行主代码。可以重复调用（状态保留在 global_ 里）。
  Result run(const Program& program);

  // 全局槽的当前值，便于测试与宿主读取。
  const std::vector<Value>& globals() const { return global_; }

 private:
  std::vector<Value> global_;
};

// 反汇编：给测试与排错用。produces 人类可读的一行行指令。
std::vector<std::string> disassemble(const Program& program);

}  // namespace vm
}  // namespace pyui
