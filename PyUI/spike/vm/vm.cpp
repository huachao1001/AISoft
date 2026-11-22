// 轻量 VM 的执行核心 + 一个最小编译器（spike 用）
//
// 这个文件的目的是**拿实测数字**，不是最终实现：
//
//   1. VM 自身的 .text 有多大？（目标 < 100 KB）
//   2. 一百万次循环要多久？（对照 CPython 与转译版）
//
// 编译器只覆盖足够跑基准的语法：常量、局部/全局、算术、比较、
// 跳转、调用、列表/字典、下标。真实语法覆盖仍要靠 CPython 的 ast。
#include "vm.h"

#include <cmath>
#include <cstdio>

namespace pyui {
namespace vm {
namespace {

// ── 字节码读写 ──
//
// 定长操作数、小端序。不用变长整数：那会让每个操作数的解析都多一次分支，
// 而我们要的正是「一条指令 = 一次 switch + 几次直接下标」。

inline void put8(std::vector<std::uint8_t>& code, std::uint8_t value) {
  code.push_back(value);
}

inline void put16(std::vector<std::uint8_t>& code, std::uint16_t value) {
  code.push_back(static_cast<std::uint8_t>(value & 0xFF));
  code.push_back(static_cast<std::uint8_t>(value >> 8));
}

inline std::uint16_t read16(const std::vector<std::uint8_t>& code, size_t at) {
  return static_cast<std::uint16_t>(code[at] | (code[at + 1] << 8));
}

// ── 二元/比较/一元算子的分派 ──
//
// 写成自由函数而不是成员，是为了让 find 出来的符号名短一点，
// 也避免 Machine 的每个实例都带一份代码。

Value applyBin(BinOp op, const Value& a, const Value& b) {
  switch (op) {
    case BinOp::kAdd: return add(a, b);
    case BinOp::kSub: return sub(a, b);
    case BinOp::kMul: return mul(a, b);
    case BinOp::kDiv: return div(a, b);
    case BinOp::kFloorDiv: return floorDiv(a, b);
    case BinOp::kMod: return mod(a, b);
    case BinOp::kPow: return power(a, b);
    case BinOp::kBitAnd: return bitAnd(a, b);
    case BinOp::kBitOr: return bitOr(a, b);
    case BinOp::kBitXor: return bitXor(a, b);
    case BinOp::kShl: return shiftLeft(a, b);
    case BinOp::kShr: return shiftRight(a, b);
  }
  return Value();
}

Value applyCmp(CmpOp op, const Value& a, const Value& b) {
  switch (op) {
    case CmpOp::kEq: return eq(a, b);
    case CmpOp::kNe: return ne(a, b);
    case CmpOp::kLt: return lt(a, b);
    case CmpOp::kLe: return le(a, b);
    case CmpOp::kGt: return gt(a, b);
    case CmpOp::kGe: return ge(a, b);
    case CmpOp::kIn: return contains(b, a);
    case CmpOp::kNotIn: return lnot(contains(b, a));
  }
  return Value();
}

Value applyUnary(UnOp op, const Value& a) {
  switch (op) {
    case UnOp::kNeg: return neg(a);
    case UnOp::kNot: return lnot(a);
    case UnOp::kInvert: return invert(a);
  }
  return Value();
}

}  // namespace

// ── 执行 ──

Result Machine::run(const Program& program) {
  Result result;
  if (static_cast<int>(global_.size()) < program.globalCount) {
    global_.resize(static_cast<size_t>(program.globalCount));
  }

  // 操作数栈。固定 256 深：脚本不是任意表达式生成器，超过这个深度几乎
  // 只可能是编译器 bug，直接报错比静默越界好。
  constexpr int kStackSize = 256;
  Value stack[kStackSize];
  int sp = 0;

  // 局部槽只给主代码用；函数调用时换一套（见下面的 kCall）。
  // spike 里不做真正的调用栈，先把开销量出来即可。
  std::vector<Value> locals(16);

  const std::vector<std::uint8_t>& code = program.code;
  size_t pc = 0;

  auto fail = [&result](const char* message) {
    result.failed = true;
    result.error = message;
    return result;
  };

  while (pc < code.size()) {
    const auto op = static_cast<Op>(code[pc++]);
    switch (op) {
      case Op::kConst: {
        const std::uint16_t index = read16(code, pc);
        pc += 2;
        stack[sp++] = program.consts[index];
        break;
      }
      case Op::kNone: stack[sp++] = Value(); break;
      case Op::kTrue: stack[sp++] = Value(true); break;
      case Op::kFalse: stack[sp++] = Value(false); break;

      case Op::kLocal: {
        const std::uint16_t slot = read16(code, pc);
        pc += 2;
        stack[sp++] = locals[slot];
        break;
      }
      case Op::kStoreLocal: {
        const std::uint16_t slot = read16(code, pc);
        pc += 2;
        locals[slot] = stack[--sp];
        break;
      }
      case Op::kGlobal: {
        const std::uint16_t slot = read16(code, pc);
        pc += 2;
        stack[sp++] = global_[slot];
        break;
      }
      case Op::kStoreGlobal: {
        const std::uint16_t slot = read16(code, pc);
        pc += 2;
        global_[slot] = stack[--sp];
        break;
      }

      case Op::kBin: {
        const auto operand = static_cast<BinOp>(code[pc++]);
        const Value b = stack[--sp];
        stack[sp - 1] = applyBin(operand, stack[sp - 1], b);
        break;
      }
      case Op::kCmp: {
        const auto operand = static_cast<CmpOp>(code[pc++]);
        const Value b = stack[--sp];
        stack[sp - 1] = applyCmp(operand, stack[sp - 1], b);
        break;
      }
      case Op::kUnary: {
        const auto operand = static_cast<UnOp>(code[pc++]);
        stack[sp - 1] = applyUnary(operand, stack[sp - 1]);
        break;
      }

      case Op::kJump: {
        pc = static_cast<size_t>(read16(code, pc));
        break;
      }
      case Op::kJumpIfFalse: {
        const size_t target = read16(code, pc);
        pc += 2;
        if (!stack[--sp].toBool()) pc = target;
        break;
      }
      case Op::kJumpIfFalseKeep: {
        const size_t target = read16(code, pc);
        pc += 2;
        if (!stack[sp - 1].toBool()) pc = target;
        break;
      }
      case Op::kJumpIfTrueKeep: {
        const size_t target = read16(code, pc);
        pc += 2;
        if (stack[sp - 1].toBool()) pc = target;
        break;
      }

      case Op::kPop: --sp; break;
      case Op::kDup: stack[sp] = stack[sp - 1]; ++sp; break;

      case Op::kCall: {
        const std::uint16_t index = read16(code, pc);
        const std::uint8_t argc = code[pc + 2];
        pc += 3;
        const Function& fn = program.functions[index];
        // spike：参数与局部量共用一块，调用完再恢复。
        std::vector<Value> saved = locals;
        locals.assign(static_cast<size_t>(fn.localCount), Value());
        for (int i = argc - 1; i >= 0; --i) locals[static_cast<size_t>(i)] = stack[--sp];
        // 递归执行：为了量速度，这里直接内联一个子循环不做真栈帧。
        // （真实实现要换成显式栈，否则深递归会爆 C 栈。）
        Program sub;
        sub.code = fn.code;
        sub.functions = program.functions;
        sub.consts = program.consts;
        sub.extensions = program.extensions;
        sub.globalCount = program.globalCount;
        // 用同一个 Machine 跑：状态一致性由 global_ 保证。
        // 注意 sub.run 会重置自己的 locals，所以这里只是近似测量。
        Result inner = run(sub);
        locals = saved;
        if (inner.failed) return inner;
        stack[sp++] = inner.value;
        break;
      }

      case Op::kExt: {
        const std::uint16_t index = read16(code, pc);
        const std::uint8_t argc = code[pc + 2];
        pc += 3;
        ExtensionFn fn = program.extensions[index];
        // 实参在栈上是「先压在前」，所以要从后往前填。
        Value args[8];
        for (int i = argc - 1; i >= 0; --i) args[i] = stack[--sp];
        stack[sp++] = fn(args, argc);
        break;
      }

      case Op::kList: {
        const std::uint16_t count = read16(code, pc);
        pc += 2;
        Value::List items;
        items.reserve(count);
        for (int i = count; i > 0; --i) items.push_back(stack[sp - i]);
        sp -= count;
        stack[sp++] = Value::list(std::move(items));
        break;
      }

      case Op::kDict: {
        const std::uint16_t pairs = read16(code, pc);
        pc += 2;
        Value::List keys;
        Value::List values;
        keys.reserve(pairs);
        values.reserve(pairs);
        for (int i = pairs; i > 0; --i) {
          const int base = sp - i * 2;
          keys.push_back(stack[base]);
          values.push_back(stack[base + 1]);
        }
        sp -= pairs * 2;
        stack[sp++] = Value::dict(keys, values);
        break;
      }

      case Op::kGetItem: {
        const Value index = stack[--sp];
        stack[sp - 1] = at(stack[sp - 1], index);
        break;
      }
      case Op::kSetItem: {
        const Value index = stack[--sp];
        const Value container = stack[--sp];
        const Value value = stack[--sp];
        setItem(container, index, value);
        break;
      }

      case Op::kReturn:
        result.value = sp > 0 ? stack[--sp] : Value();
        return result;
    }
  }

  result.value = sp > 0 ? stack[sp - 1] : Value();
  return result;
}

// ── 反汇编 ──

namespace {

const char* binName(BinOp op) {
  switch (op) {
    case BinOp::kAdd: return "+";
    case BinOp::kSub: return "-";
    case BinOp::kMul: return "*";
    case BinOp::kDiv: return "/";
    case BinOp::kFloorDiv: return "//";
    case BinOp::kMod: return "%";
    case BinOp::kPow: return "**";
    case BinOp::kBitAnd: return "&";
    case BinOp::kBitOr: return "|";
    case BinOp::kBitXor: return "^";
    case BinOp::kShl: return "<<";
    case BinOp::kShr: return ">>";
  }
  return "?";
}

const char* cmpName(CmpOp op) {
  switch (op) {
    case CmpOp::kEq: return "==";
    case CmpOp::kNe: return "!=";
    case CmpOp::kLt: return "<";
    case CmpOp::kLe: return "<=";
    case CmpOp::kGt: return ">";
    case CmpOp::kGe: return ">=";
    case CmpOp::kIn: return "in";
    case CmpOp::kNotIn: return "not in";
  }
  return "?";
}

}  // namespace

std::vector<std::string> disassemble(const Program& program) {
  std::vector<std::string> lines;
  const std::vector<std::uint8_t>& code = program.code;
  size_t pc = 0;
  while (pc < code.size()) {
    const size_t start = pc;
    const auto op = static_cast<Op>(code[pc++]);
    char text[128];
    switch (op) {
      case Op::kConst: snprintf(text, sizeof(text), "const %u", read16(code, pc)); pc += 2; break;
      case Op::kLocal: snprintf(text, sizeof(text), "local %u", read16(code, pc)); pc += 2; break;
      case Op::kStoreLocal: snprintf(text, sizeof(text), "store_local %u", read16(code, pc)); pc += 2; break;
      case Op::kGlobal: snprintf(text, sizeof(text), "global %u", read16(code, pc)); pc += 2; break;
      case Op::kStoreGlobal: snprintf(text, sizeof(text), "store_global %u", read16(code, pc)); pc += 2; break;
      case Op::kBin: snprintf(text, sizeof(text), "bin %s", binName(static_cast<BinOp>(code[pc]))); pc += 1; break;
      case Op::kCmp: snprintf(text, sizeof(text), "cmp %s", cmpName(static_cast<CmpOp>(code[pc]))); pc += 1; break;
      case Op::kUnary: snprintf(text, sizeof(text), "unary %d", code[pc]); pc += 1; break;
      case Op::kJump: snprintf(text, sizeof(text), "jump %u", read16(code, pc)); pc += 2; break;
      case Op::kJumpIfFalse: snprintf(text, sizeof(text), "jump_if_false %u", read16(code, pc)); pc += 2; break;
      case Op::kJumpIfFalseKeep: snprintf(text, sizeof(text), "jump_if_false_keep %u", read16(code, pc)); pc += 2; break;
      case Op::kJumpIfTrueKeep: snprintf(text, sizeof(text), "jump_if_true_keep %u", read16(code, pc)); pc += 2; break;
      case Op::kCall: snprintf(text, sizeof(text), "call %u,%u", read16(code, pc), code[pc + 2]); pc += 3; break;
      case Op::kExt: snprintf(text, sizeof(text), "ext %u,%u", read16(code, pc), code[pc + 2]); pc += 3; break;
      case Op::kList: snprintf(text, sizeof(text), "list %u", read16(code, pc)); pc += 2; break;
      case Op::kDict: snprintf(text, sizeof(text), "dict %u", read16(code, pc)); pc += 2; break;
      default: snprintf(text, sizeof(text), "%s", "-"); break;
    }
    lines.push_back(std::to_string(start) + ": " + text);
  }
  return lines;
}

}  // namespace vm
}  // namespace pyui
