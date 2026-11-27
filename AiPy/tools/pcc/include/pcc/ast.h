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
  ExceptHandler,  // try 的一个 except 子句
  Match,
  Case,           // match 的一个 case 子句
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
  // 字节串常量。与 Str 分开：加载时构造的是 Value::bytes 而不是 Value(string)，
  // 否则 b"abc" == "abc" 会成立（CPython 里是 False）。
  Bytes = 5,
  // `...`：常量池里放一个「无载荷」的标记，加载时构造 Value::ellipsis()。
  Ellipsis = 6,
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
  // 装饰器个数。装饰器表达式本身存在 decorators 里，按源码顺序（最靠近
  // def 的最后一个，应用顺序是从下往上）。
  std::size_t decoratorCount = 0;
  int decoratorLine = 0;
  int decoratorCol = 0;
  std::vector<NodePtr> decorators;

  std::vector<std::string> names;  // Global.names
  std::vector<Kind> ops;           // 算子序列
  std::vector<int> opLines;        // 与 ops 平行，报错定位用

  // 切片哪几段写了。1=start 2=stop 4=step，与 SLICE 指令的位标记一致。
  int sliceFlags = 0;

  // ── Try 专用 ──
  //
  // 与 Python 的 ast.Try 对齐：children 按
  //   [body..., handler1, handler2, ..., orelse..., finalbody...]
  // 排放，靠下面三个计数切段。handler 本身是一个 Kind::ExceptHandler 节点，
  // 它的 text 存 `as` 绑定的名字（空串表示没写 as），children 是
  // [异常类型表达式（可能没有）, handler 体...]。
  //
  // 为什么 body/handler/orelse/final 不拆成四个 vector：Node 只有一个
  // children，拆四个会让每个节点多 4 个 vector（24 字节 × 4）。而 try 在
  // 脚本里是低频结构，用计数切段够了——这与 If 的 bodyCount/orelseCount
  // 是同一条做法。
  std::size_t handlerCount = 0;   // except 子句个数
  std::size_t finalCount = 0;     // finally 体的语句数
  std::size_t handlerBodyCount = 0;  // ExceptHandler 自己的 body 语句数
  bool hasExceptVar = false;      // ExceptHandler 写了 `as name`
  bool hasType = false;           // ExceptHandler 写了异常类型（否则是裸 except）

  // Arguments 专用
  std::vector<std::string> params;
  // 仅关键字参数（`def f(*, a)` 里的 a）。它们在 params 之后单独占槽。
  std::vector<std::string> kwonlyParams;
  // 与 params 平行的默认值表达式：非空表示这个位置参数带默认值。
  // CPython 允许 `def f(a=1, b=2)` 只能尾部连续，所以这在位置段里等价于
  // 「尾部个数」，但独立存一份让「有没有默认值」不依赖位置推断。
  std::vector<NodePtr> paramDefaults;
  // 与 kwonlyParams 平行：仅关键字参数的默认值可以不在尾部
  // （`def f(*, a=1, b)` 合法），所以必须逐个记。
  std::vector<NodePtr> kwonlyDefaults;
  // `*args` / `**kwargs` 的绑定名（空串表示没写）。
  std::string varargName;
  std::string kwargName;
  // 从哪些下标开始是仅关键字参数（相对 params 的长度：kwonly 全排在后面）。
  bool hasVararg = false;
  bool hasKwarg = false;
  bool hasKwonly = false;
  bool hasPosonly = false;
  // `/` 之前的参数个数（也就是位置-only 参数个数）。
  int posonlyCount = 0;
  int posonlyLine = 0;
  int posonlyCol = 0;
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
