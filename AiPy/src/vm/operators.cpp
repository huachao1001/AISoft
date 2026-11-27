// 算子分派：把字节码里的 BinOp/CmpOp/UnOp 映射到 Value 的语义函数。
//
// ── 为什么这一层只有 switch ──
//
// `Value` 已经把所有语义做完了（`add` 懂 int/float/str/list 的重载规则）。
// 这里唯一的价值是把「字节码枚举」翻译成「C++ 函数」，别的一概不做。
// 一旦有人想在这里加「先转字符串再拼」之类的聪明处理，那条规则就该搬到
// `value.cpp`——否则 AOT 路线（直接调 `py::add`）会与 VM 行为分叉。
#include "vm/internal.h"

namespace aipy {
namespace vm {
namespace detail {

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
    case BinOp::kMatMult: return matMult(a, b);
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
    // `in` 的栈顺序是 [元素, 容器]（`a in b` 先求 a 再求 b），而 contains
    // 的签名是 contains(容器, 元素)，所以这里要换位。
    case CmpOp::kIn: return contains(b, a);
    case CmpOp::kNotIn: return lnot(contains(b, a));
    case CmpOp::kIs: return isSame(a, b);
    case CmpOp::kIsNot: return lnot(isSame(a, b));
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

}  // namespace detail
}  // namespace vm
}  // namespace aipy
