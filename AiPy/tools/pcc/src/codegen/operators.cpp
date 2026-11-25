// 算子映射：AST 算子 → 字节码编号。
//
// 编号必须与 AiPy/include/aipy/vm.h 的 BinOp/CmpOp/UnOp 同序。名字表在
// tables.cpp，这里只负责「AST 算子 → 名字」这一层。分开的理由：报错要用
// Python 的 ast 类名（`MatMult`），而编号要用 VM 的名字（`MUL`）。
#include "pcc/codegen/codegen.h"

#include "pcc/codegen/tables.h"
#include "pcc/diagnostics.h"

namespace pcc {

int CodeGen::binopOf(Kind op, const Node& node) {
  const int number = Tables::binopNumber(op);
  if (number >= 0) return number;
  failAt(diag::unsupportedOperator(kind_name(op)), node);
  return 0;
}

int CodeGen::cmpopOf(Kind op, const Node& node) {
  const int number = Tables::cmpopNumber(op);
  if (number >= 0) return number;
  failAt(diag::unsupportedComparison(kind_name(op)), node);
  return 0;
}

int CodeGen::unopOf(Kind op, const Node& node) {
  const int number = Tables::unopNumber(op);
  if (number >= 0) return number;
  failAt(diag::unsupportedUnary(kind_name(op)), node);
  return 0;
}

}  // namespace pcc
