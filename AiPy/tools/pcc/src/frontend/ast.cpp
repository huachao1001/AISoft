// 节点种类的名字。用 Python `ast` 的类名，报错信息直接引用它们。
#include "pcc/ast.h"

namespace pcc {

const char* kind_name(Kind kind) {
  switch (kind) {
    // 语句
    case Kind::Module: return "Module";
    case Kind::StatementList: return "StatementList";
    case Kind::Expr: return "Expr";
    case Kind::Assign: return "Assign";
    case Kind::AnnAssign: return "AnnAssign";
    case Kind::AugAssign: return "AugAssign";
    case Kind::Return: return "Return";
    case Kind::If: return "If";
    case Kind::While: return "While";
    case Kind::For: return "For";
    case Kind::Break: return "Break";
    case Kind::Continue: return "Continue";
    case Kind::Global: return "Global";
    case Kind::Nonlocal: return "Nonlocal";
    case Kind::Pass: return "Pass";
    case Kind::With: return "With";
    case Kind::WithItem: return "WithItem";
    case Kind::Import: return "Import";
    case Kind::ImportFrom: return "ImportFrom";
    case Kind::Alias: return "alias";
    case Kind::FunctionDef: return "FunctionDef";
    case Kind::ClassDef: return "ClassDef";
    case Kind::Delete: return "Delete";
    case Kind::Assert: return "Assert";
    case Kind::Raise: return "Raise";
    case Kind::Try: return "Try";
    case Kind::Match: return "Match";
    case Kind::AsyncFunctionDef: return "AsyncFunctionDef";
    case Kind::AsyncFor: return "AsyncFor";
    case Kind::AsyncWith: return "AsyncWith";
    case Kind::TypeAlias: return "TypeAlias";
    // 表达式
    case Kind::Constant: return "Constant";
    case Kind::Name: return "Name";
    case Kind::Attribute: return "Attribute";
    case Kind::BinOp: return "BinOp";
    case Kind::UnaryOp: return "UnaryOp";
    case Kind::BoolOp: return "BoolOp";
    case Kind::Compare: return "Compare";
    case Kind::Call: return "Call";
    case Kind::Keyword: return "keyword";
    case Kind::IfExp: return "IfExp";
    case Kind::Subscript: return "Subscript";
    case Kind::Slice: return "Slice";
    case Kind::ListLit: return "List";
    case Kind::DictLit: return "Dict";
    case Kind::TupleLit: return "Tuple";
    case Kind::SetLit: return "Set";
    case Kind::JoinedStr: return "JoinedStr";
    case Kind::Lambda: return "Lambda";
    case Kind::NamedExpr: return "NamedExpr";
    case Kind::ListComp: return "ListComp";
    case Kind::SetComp: return "SetComp";
    case Kind::DictComp: return "DictComp";
    case Kind::GeneratorExp: return "GeneratorExp";
    case Kind::Starred: return "Starred";
    case Kind::Await: return "Await";
    case Kind::Yield: return "Yield";
    case Kind::YieldFrom: return "YieldFrom";
    case Kind::Arguments: return "arguments";
    case Kind::Arg: return "arg";
    // 算子
    case Kind::Add: return "Add";
    case Kind::Sub: return "Sub";
    case Kind::Mult: return "Mult";
    case Kind::Div: return "Div";
    case Kind::FloorDiv: return "FloorDiv";
    case Kind::Mod: return "Mod";
    case Kind::Pow: return "Pow";
    case Kind::BitAnd: return "BitAnd";
    case Kind::BitOr: return "BitOr";
    case Kind::BitXor: return "BitXor";
    case Kind::LShift: return "LShift";
    case Kind::RShift: return "RShift";
    case Kind::MatMult: return "MatMult";
    case Kind::Eq: return "Eq";
    case Kind::NotEq: return "NotEq";
    case Kind::Lt: return "Lt";
    case Kind::LtE: return "LtE";
    case Kind::Gt: return "Gt";
    case Kind::GtE: return "GtE";
    case Kind::In: return "In";
    case Kind::NotIn: return "NotIn";
    case Kind::Is: return "Is";
    case Kind::IsNot: return "IsNot";
    case Kind::USub: return "USub";
    case Kind::UAdd: return "UAdd";
    case Kind::Not: return "Not";
    case Kind::Invert: return "Invert";
    case Kind::And: return "And";
    case Kind::Or: return "Or";
  }
  return "?";
}

}  // namespace pcc
