// AiPy 编译器的抽象语法树。
//
// ── 为什么要自己写 AST ──
//
// Python 版的编译器靠 `ast.parse()` 拿树。换到 C++ 就没有那个模块了：CPython
// 的 PEG 解析器是它自己的实现，没有可复用的库。而 AiPy 要让用户「不装 Python
// 也能打包」，编译器就必须自带解析器。
//
// ── 设计取向：贴住 Python 的 ast 形状 ──
//
// 这里不发明「更漂亮」的树，而是逐节点对齐 `ast` 的字段名与语义。理由是
// 代码生成部分要从 compiler.py 直译过来，两边形状一致才能一段一段对照着
// 抄，也才能在写错时保持同样的行为。
//
// 用一个通用的 Node 而不是一堆 class：节点种类是封闭的（约 40 种，由
// LANGUAGE.md 的子集定义），分派靠 `kind`。几十种 class 加虚函数在这里换不到
// 任何好处，只会让「直译」变成「重新设计」。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pcc {

// 节点种类。名字与 Python `ast` 的类名一一对应——报错信息里会直接出现
// 它们（例如 "unsupported expression: Set"），所以不能改名。
enum class Kind {
  // ── 语句 ──
  Module,
  StatementList,  // 一行多条语句 `a = 1; b = 2`，摊平前用
  Expr,           // 表达式语句
  Assign,         // a = b = v
  AnnAssign,      // x: int = v
  AugAssign,      // x += v
  Return,
  If,
  While,
  For,
  Break,
  Continue,
  Global,
  Nonlocal,
  Pass,
  With,
  WithItem,   // With 的一项
  Import,     // import a, b as c
  ImportFrom, // from m import x
  Alias,      // import 里的一个名字
  FunctionDef,
  ClassDef,
  // 需要明确拒绝的语句（保留种类是为了给出准确提示）
  Delete,
  Assert,
  Raise,
  Try,
  Match,
  AsyncFunctionDef,
  AsyncFor,
  AsyncWith,
  TypeAlias,

  // ── 表达式 ──
  Constant,   // 数字 / 字符串 / True / False / None / ...
  Name,       // 标识符
  Attribute,  // a.b
  BinOp,      // a + b
  UnaryOp,    // -a
  BoolOp,     // a and b
  Compare,    // a < b < c
  Call,       // f(...)
  Keyword,    // 调用里的 name=value
  IfExp,      // a if c else b
  Subscript,  // a[i]
  Slice,      // a[lo:hi:step] 里的切片
  ListLit,
  DictLit,
  TupleLit,
  SetLit,
  JoinedStr,  // f-string
  Lambda,
  NamedExpr,  // 海象 :=
  ListComp,
  SetComp,
  DictComp,
  GeneratorExp,
  Starred,
  Await,
  Yield,
  YieldFrom,
  Arguments,  // FunctionDef 的参数列表
  Arg,        // 一个参数

  // ── 算子（做成节点，与 Python 的 ast.op 对照）──
  Add, Sub, Mult, Div, FloorDiv, Mod, Pow,
  BitAnd, BitOr, BitXor, LShift, RShift, MatMult,
  Eq, NotEq, Lt, LtE, Gt, GtE, In, NotIn, Is, IsNot,
  USub, UAdd, Not, Invert,
  And, Or,
};

// 常量类型，与 pack.py 的 CONST_* 一致。
enum class ConstKind : std::uint8_t {
  None = 0,
  Bool = 1,
  Int = 2,
  Float = 3,
  Str = 4,
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node {
  Kind kind = Kind::Module;
  int line = 0;  // 1-based，与 Python 的 lineno 一致
  int col = 0;   // 0-based，与 Python 的 col_offset 一致

  // ── 通用载荷 ──
  // 不同 kind 用其中一部分。合成一个结构而不是继承：节点数量大，继承加
  // 虚析构会让每个节点多一个 vtable 指针，而这个工具对体积敏感。
  std::string text;  // Name.id / Attribute.attr / FunctionDef.name / 字符串值
  std::string raw;   // Alias 的 as 名；其它节点不用
  long long intValue = 0;
  ConstKind constKind = ConstKind::None;
  bool flag = false;  // 通用布尔位（bytes 字面量、复数、相对导入）
  bool selfAttr = false;  // Attribute 的接收者是 `self`

  // 子节点。各 kind 的语义见 diagnostics 与 codegen 的注释。
  std::vector<NodePtr> children;

  // 并列列表的切分计数。
  std::size_t bodyCount = 0;    // If/While/For/FunctionDef/ClassDef/With 的 body
  std::size_t orelseCount = 0;  // If/While/For 的 orelse
  std::size_t argsCount = 0;    // Call 的位置参数个数 / ClassDef 的基类个数 / With 的项数
  std::size_t keyCount = 0;     // DictLit 的键个数
  // 装饰器个数。内容不保留（全部拒绝），但「有几个」要能报出来，
  // 因为它决定了报错该指向装饰器还是指向定义。
  std::size_t decoratorCount = 0;
  int decoratorLine = 0;
  int decoratorCol = 0;

  std::vector<std::string> names;  // Global.names
  std::vector<Kind> ops;           // 算子序列
  std::vector<int> opLines;        // 与 ops 平行，报错定位用

  // 切片哪几段写了。1=start 2=stop 4=step，与 SLICE 指令的位标记一致。
  int sliceFlags = 0;

  // Arguments 专用
  std::vector<std::string> params;
  std::size_t defaults = 0;
  bool hasVararg = false;
  bool hasKwarg = false;
  bool hasKwonly = false;
  bool hasPosonly = false;
  int posonlyLine = 0;
  int posonlyCol = 0;
  int defaultLine = 0;
  int defaultCol = 0;
};

inline NodePtr make_node(Kind kind, int line, int col) {
  auto node = std::make_unique<Node>();
  node->kind = kind;
  node->line = line;
  node->col = col;
  return node;
}

// Python `ast` 的类名，报错信息里要用（例如 "Set"、"NamedExpr"）。
const char* kind_name(Kind kind);

}  // namespace pcc
