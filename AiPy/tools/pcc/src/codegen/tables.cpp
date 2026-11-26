// 字节码表的实现。
#include "pcc/codegen/tables.h"

#include <cstring>

namespace pcc {
namespace {

// 与 vm.h 的 Op 枚举同序。
const char* const kOpNames[] = {
    "CONST", "NONE", "TRUE", "FALSE",
    "LOAD_LOCAL", "STORE_LOCAL", "LOAD_GLOBAL", "STORE_GLOBAL",
    "BIN", "CMP", "UNARY",
    "JUMP", "JUMP_IF_FALSE", "JUMP_IF_FALSE_KEEP", "JUMP_IF_TRUE_KEEP",
    "POP", "DUP",
    "CALL", "CALL_BUILTIN", "CALL_METHOD", "CALL_EXT",
    "RETURN",
    "BUILD_LIST", "BUILD_DICT",
    "GET_ITEM", "SET_ITEM", "SLICE", "SET_SLICE",
    "ITER_INIT", "ITER_NEXT", "ITER_BREAK",
    "INPLACE",
    "WITH_ENTER", "WITH_EXIT",
    "SETUP_EXC", "POP_EXC", "RAISE", "RAISE_TYPE", "RERAISE",
    "EXC_MATCH", "CURRENT_EXC", "CLEAR_EXC",
    "DEL_LOCAL", "DEL_GLOBAL", "BUILD_TUPLE", "BUILD_SET",
    // 追加在末尾：Op 的数值就是字节码里的操作码，插到中间会让已生成的
    // .bc 与 .pylib 内的字节码被执行成别的指令。
    "CALL_DYN",
    "DICT_MERGE",
    "MAKE_FUNCTION",
    "CALL_VALUE",
    "CALL_VALUE_DYN",
    "MAKE_CLASS",
    "GET_ATTR",
    "SET_ATTR",
    "INSTANTIATE",
    "MAKE_SUPER",
};

constexpr int kOpCount = static_cast<int>(sizeof(kOpNames) / sizeof(kOpNames[0]));

// 与 src/builtins.cpp 的 kBuiltinTable 同序。
const char* const kBuiltins[] = {
    "len", "str", "int", "float", "bool", "abs", "round", "min", "max",
    "sum", "list", "dict", "range", "print",
    "format_text", "char_from_code", "text_of_char", "set",
};

constexpr int kBuiltinCount =
    static_cast<int>(sizeof(kBuiltins) / sizeof(kBuiltins[0]));

// 与 vm.h 的 BinOp / CmpOp / UnOp 同序。
const char* const kBinOps[] = {
    "ADD", "SUB", "MUL", "DIV", "FLOOR_DIV", "MOD", "POW",
    "BIT_AND", "BIT_OR", "BIT_XOR", "SHL", "SHR",
};
const char* const kCmpOps[] = {"EQ", "NE", "LT", "LE", "GT", "GE", "IN", "NOT_IN",
                                "IS", "IS_NOT"};
const char* const kUnOps[] = {"NEG", "NOT", "INVERT"};

int findIn(const char* const* table, int count, const char* name) {
  for (int i = 0; i < count; ++i) {
    if (std::strcmp(table[i], name) == 0) return i;
  }
  return -1;
}

}  // namespace

const char* const* Tables::opNames() { return kOpNames; }
int Tables::opCount() { return kOpCount; }

int Tables::opByName(const char* name) { return findIn(kOpNames, kOpCount, name); }

const char* const* Tables::builtinNames() { return kBuiltins; }
int Tables::builtinCount() { return kBuiltinCount; }

bool Tables::isBuiltin(const std::string& name) {
  for (int i = 0; i < kBuiltinCount; ++i) {
    if (name == kBuiltins[i]) return true;
  }
  return false;
}

int Tables::binopNumber(Kind op) {
  const char* spelling = binopSpelling(op);
  if (spelling == nullptr) return -1;
  return findIn(kBinOps, 12, spelling);
}

int Tables::cmpopNumber(Kind op) {
  const char* spelling = cmpopSpelling(op);
  if (spelling == nullptr) return -1;
  return findIn(kCmpOps, 10, spelling);
}

int Tables::unopNumber(Kind op) {
  const char* spelling = unopSpelling(op);
  if (spelling == nullptr) return -1;
  return findIn(kUnOps, 3, spelling);
}

const char* Tables::binopSpelling(Kind op) {
  switch (op) {
    case Kind::Add: return "ADD";
    case Kind::Sub: return "SUB";
    case Kind::Mult: return "MUL";
    case Kind::Div: return "DIV";
    case Kind::FloorDiv: return "FLOOR_DIV";
    case Kind::Mod: return "MOD";
    case Kind::Pow: return "POW";
    case Kind::BitAnd: return "BIT_AND";
    case Kind::BitOr: return "BIT_OR";
    case Kind::BitXor: return "BIT_XOR";
    case Kind::LShift: return "SHL";
    case Kind::RShift: return "SHR";
    case Kind::MatMult: return "MatMult";
    default: return nullptr;
  }
}

const char* Tables::cmpopSpelling(Kind op) {
  switch (op) {
    case Kind::Eq: return "EQ";
    case Kind::NotEq: return "NE";
    case Kind::Lt: return "LT";
    case Kind::LtE: return "LE";
    case Kind::Gt: return "GT";
    case Kind::GtE: return "GE";
    case Kind::In: return "IN";
    case Kind::NotIn: return "NOT_IN";
    case Kind::Is: return "IS";
    case Kind::IsNot: return "IS_NOT";
    default: return nullptr;  }
}

const char* Tables::unopSpelling(Kind op) {
  switch (op) {
    case Kind::USub: return "NEG";
    case Kind::Not: return "NOT";
    case Kind::Invert: return "INVERT";
    case Kind::UAdd: return "UAdd";
    default: return nullptr;
  }
}

}  // namespace pcc
