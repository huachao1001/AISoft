// 反汇编：把字节码渲染成「偏移: 指令 操作数」，给人看。
//
// ── 为什么反汇编要能解码每一条指令 ──
//
// 它的真正用途不是「看看」，而是**校验**。`tools/aipyic/aipyic.py` 在
// Python 侧独立解码同一个 `.bc`：两边对不上就说明操作数长度或字段顺序
// 有分歧——这正是最容易被忽略、后果最严重的一类 bug（静默错位）。
//
// 所以这里的 switch 必须与执行循环的操作数读取逐条对齐。少写一条偏移，
// 后面的指令会整体错位，而反汇编本身不会报错，只会打出垃圾。
#include "aipy/vm.h"

#include <cstdio>
#include <string>
#include <vector>

namespace aipy {
namespace vm {
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
    case CmpOp::kIs: return "is";
    case CmpOp::kIsNot: return "is not";
  }
  return "?";
}

std::uint16_t read16(const std::vector<std::uint8_t>& code, std::size_t at) {
  return static_cast<std::uint16_t>(code[at] | (code[at + 1] << 8));
}

}  // namespace

std::vector<std::string> disassemble(const Program& program) {
  std::vector<std::string> lines;
  const std::vector<std::uint8_t>& code = program.code;
  std::size_t pc = 0;
  while (pc < code.size()) {
    const std::size_t start = pc;
    const auto op = static_cast<Op>(code[pc++]);
    char text[160];
    switch (op) {
      case Op::kConst:
        std::snprintf(text, sizeof(text), "const %u", read16(code, pc));
        pc += 2;
        break;
      case Op::kLoadLocal:
      case Op::kStoreLocal:
      case Op::kLoadGlobal:
      case Op::kStoreGlobal:
      case Op::kBuildList:
      case Op::kBuildDict:
        std::snprintf(text, sizeof(text), "%s %u", opName(op), read16(code, pc));
        pc += 2;
        break;
      case Op::kBin:
        std::snprintf(text, sizeof(text), "bin %s", binName(static_cast<BinOp>(code[pc])));
        pc += 1;
        break;
      case Op::kCmp:
        std::snprintf(text, sizeof(text), "cmp %s", cmpName(static_cast<CmpOp>(code[pc])));
        pc += 1;
        break;
      case Op::kUnary:
        std::snprintf(text, sizeof(text), "unary %d", code[pc]);
        pc += 1;
        break;
      case Op::kJump:
      case Op::kJumpIfFalse:
      case Op::kJumpIfFalseKeep:
      case Op::kJumpIfTrueKeep:
      case Op::kIterNext:
        std::snprintf(text, sizeof(text), "%s %u", opName(op), read16(code, pc));
        pc += 2;
        break;
      case Op::kCall:
      case Op::kCallBuiltin:
      case Op::kCallExt:
        std::snprintf(text, sizeof(text), "%s %u,%u", opName(op), read16(code, pc), code[pc + 2]);
        pc += 3;
        break;
      case Op::kCallDyn:
        std::snprintf(text, sizeof(text), "call_dyn %u", read16(code, pc));
        pc += 2;
        break;
      case Op::kDictMerge:
        std::snprintf(text, sizeof(text), "dict_merge");
        break;
      case Op::kMakeFunction:
        std::snprintf(text, sizeof(text), "make_function %u,%u",
                      static_cast<unsigned>(read16(code, pc)),
                      static_cast<unsigned>(read16(code, pc + 2)));
        pc += 4;
        break;
      case Op::kCallValue:
        std::snprintf(text, sizeof(text), "call_value %u", code[pc]);
        pc += 1;
        break;
      case Op::kCallValueDyn:
        std::snprintf(text, sizeof(text), "call_value_dyn");
        break;
      case Op::kMakeClass:
        std::snprintf(text, sizeof(text), "make_class %u,%u,%u", read16(code, pc),
                      read16(code, pc + 2), read16(code, pc + 4));
        pc += 6 + static_cast<std::size_t>(read16(code, pc + 2)) * 2;
        break;
      case Op::kGetAttr:
        std::snprintf(text, sizeof(text), "get_attr %u", read16(code, pc));
        pc += 2;
        break;
      case Op::kSetAttr:
        std::snprintf(text, sizeof(text), "set_attr %u", read16(code, pc));
        pc += 2;
        break;
      case Op::kInstantiate:
        std::snprintf(text, sizeof(text), "instantiate %u", code[pc]);
        pc += 1;
        break;
      case Op::kMakeSuper:
        std::snprintf(text, sizeof(text), "make_super %u", read16(code, pc));
        pc += 2;
        break;
      case Op::kYield:
        std::snprintf(text, sizeof(text), "yield");
        break;
      case Op::kCallMethod:
        std::snprintf(text, sizeof(text), "call_method %u,%u", read16(code, pc), code[pc + 2]);
        pc += 3;
        break;
      case Op::kSlice:
        std::snprintf(text, sizeof(text), "slice flags=%u", code[pc]);
        pc += 1;
        break;
      case Op::kSetSlice:
        std::snprintf(text, sizeof(text), "set_slice flags=%u", code[pc]);
        pc += 1;
        break;
      case Op::kSetupExc:
        std::snprintf(text, sizeof(text), "setup_exc %u", read16(code, pc));
        pc += 2;
        break;
      case Op::kRaiseType:
        std::snprintf(text, sizeof(text), "raise_type %u,msg=%u", read16(code, pc),
                      code[pc + 2]);
        pc += 3;
        break;
      case Op::kExcMatch:
      case Op::kDelLocal:
      case Op::kDelGlobal:
        std::snprintf(text, sizeof(text), "%s %u,%u", opName(op), read16(code, pc),
                      read16(code, pc + 2));
        pc += 4;
        break;
      default:
        std::snprintf(text, sizeof(text), "%s", opName(op));
        break;
    }
    lines.push_back(std::to_string(start) + ": " + text);
  }
  return lines;
}

}  // namespace vm
}  // namespace aipy
