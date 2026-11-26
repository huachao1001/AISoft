// 代码生成：AST → 字节码。
//
// ── 与 compiler.py 的关系 ──
//
// 这是 `tools/aipyic/compiler.py` 的逐段直译。判断移植是否正确的标准不是
// 「看着像」，而是**字节码逐字节相同**：同一份脚本，两个编译器产出的 .bc
// 必须完全一样。tests/check_compiler_parity.py 拿 Python 版当 oracle 守着它。
//
// ── 不复制 AST 的理由 ──
//
// Python 版用 `ast.NodeTransformer` 把方法体里的 `self.x` 改写成 `x`，再编译。
// 这里不复制树，而是靠解析期记下的 `Attribute.selfAttr` 加一个「当前在方法里」
// 的标记在生成时等价处理。槽号、指令序列都不变，省掉一次深拷贝。
//
// 但 `self.x` 只在方法体里有这个含义：模块级的 `self.x = 1` 在 Python 里
// 仍然报「不支持属性访问」。所以标记必须真的跟着编译上下文走，不能只看
// 语法形态。
#pragma once

#include "pcc/ast.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pcc {

struct CompileError {
  std::string message;
  int line = 0;
  int col = 0;
};

// 一个函数的产出。
struct CompiledBlock {
  std::string name;
  std::vector<std::uint8_t> code;
  std::vector<std::pair<std::uint32_t, int>> lines;  // (offset, line)
  int paramCount = 0;
  int localCount = 0;
};

// 常量。类型与值分开存，写二进制时按类型序列化。
struct Constant {
  ConstKind kind = ConstKind::None;
  long long intValue = 0;
  double floatValue = 0.0;
  std::string strValue;
};

// 完整编译结果，字段与 pack.py 读的那份 dict 一一对应。
struct Program {
  std::string moduleName;
  std::vector<std::uint8_t> moduleCode;
  std::vector<std::pair<std::uint32_t, int>> moduleLines;
  int moduleLocalCount = 0;
  int globalCount = 0;
  std::vector<std::string> globals;  // 按槽号排列
  std::vector<Constant> consts;
  std::vector<CompiledBlock> functions;
  std::vector<std::string> imports;
  std::vector<std::string> extensions;
  std::vector<std::string> builtins;
  // 脚本里有没有 `class X(Activity)`。
  //
  // 打包时据此决定产物子系统：有界面就是 GUI 程序（双击不弹控制台），
  // 没有就是控制台程序（双击弹出控制台看输出）。这是编译器唯一知道的
  // 「这是不是一个图形界面应用」的信号——`Activity` 是语言里唯一的类形态。
  //
  // 不进字节码：字节码格式要与 Python 版逐字节一致，而 Python 版没有这个
  // 概念。它只在 C++ 编译期到打包期之间传递。
  bool hasActivity = false;
};

class CodeGen {
 public:
  CodeGen(std::string filename, std::string moduleName);

  // 成功返回 true。失败时 error() 里有带位置的错误。
  bool compile(const Node& module);
  const Program& program() const { return program_; }
  const CompileError& error() const { return error_; }

 private:
  struct Block {
    std::string name;
    std::vector<std::uint8_t> code;
    std::vector<std::pair<std::uint32_t, int>> lines;
    int paramCount = 0;
    int localCount = 0;
    std::vector<std::string> localNames;  // 槽号 → 名字
  };

  struct LoopFrame {
    int continueTarget = 0;
    std::vector<int> breakFixups;
    bool isFor = false;
  };

  struct FunctionEntry {
    const Node* node = nullptr;
    int blockIndex = -1;
    bool isMethod = false;
    std::set<std::string> selfAssigned;
  };

  // ── 发射（emit.cpp）──
  void emit(const char* op, const Node& node);
  void emitU8(int value);
  void emitU16(int value);
  void patchU16(std::size_t at, int value);
  void emitOpNoLine(const char* op);
  int slotOf(const std::string& name);
  int allocSlot();
  int slotOfIn(Block* block, const std::string& name);

  bool fail(const std::string& message, int line, int col);
  bool failAt(const std::string& message, const Node& node);

  // ── 池与下标（emit.cpp）──
  int extensionIndex(const std::string& qualified);
  int builtinIndex(const std::string& name);
  int constIndexOfInt(long long value);
  int constIndexOfFloat(double value);
  int constIndexOfStr(const std::string& value);
  int globalSlot(const std::string& name);
  int functionIndex(const std::string& name) const;

  // ── 作用域（scope.cpp）──
  bool isLocal(const std::string& name) const;
  void nameTarget(const std::string& name, const Node& node);
  void nameStore(const std::string& name, const Node& node);
  // selfNames：是否把 `self.x` 当名字 `x`。只有方法体里为真——模块级的
  // `self.x = 1` 在 Python 里依旧是「不支持属性访问」。
  std::vector<std::string> targetNames(const Node& target, bool selfNames) const;
  std::vector<std::string> assignedNames(const Node& node, bool selfNames) const;
  static void collectGlobals(const Node& node, std::set<std::string>* out);
  static void collectSelfAssigned(const Node& node, std::set<std::string>* out);
  // 按 `ast.walk` 的 BFS 顺序收集赋值目标：槽号顺序取决于它，
  // 换个遍历顺序就会产出不同的字节码。
  void collectAssigned(const Node& node, bool selfNames,
                       std::vector<std::pair<std::string, const Node*>>* out)
      const;

  // ── 模块与函数（module.cpp）──
  bool compileModule(const Node& module);
  bool expandActivity(const Node& classNode, std::vector<const Node*>* body);
  bool registerFunction(const Node& node);
  bool compileFunctionBody(const Node& node);

  // ── 语句（statements.cpp）──
  bool statement(const Node& node);
  bool withStatement(const Node& node);
  void emitWithExits(const Node& node);
  bool ifChain(const Node& node);
  bool assignTarget(const Node& target, const Node& node);
  bool augmented(const Node& node);
  bool forLoop(const Node& node);
  bool importStatement(const Node& node);
  bool raiseStatement(const Node& node);
  bool tryStatement(const Node& node);

  // ── 表达式（expressions.cpp）──
  bool expr(const Node& node);
  // 读一个名字。`self.x` 在方法体里等价于 `x`，所以两条路径共用这一份。
  bool nameRead(const std::string& name, const Node& node);
  bool sliceExpr(const Node& node, const Node& parent);
  bool call(const Node& node);
  bool constant(const Node& node);

  // ── 算子映射（operators.cpp）──
  int binopOf(Kind op, const Node& node);
  int cmpopOf(Kind op, const Node& node);
  int unopOf(Kind op, const Node& node);

  // `self.x` 是否等价于名字 `x`：只在方法体内成立。
  bool isSelfName(const Node& node) const {
    return inMethod_ && node.kind == Kind::Attribute && node.selfAttr;
  }

  std::string filename_;
  Program program_;
  CompileError error_;

  Block module_;
  Block* block_ = &module_;

  std::vector<Block> blocks_;
  std::vector<FunctionEntry> functions_;
  std::vector<std::string> functionNames_;

  std::vector<std::string> globalOrder_;
  std::set<std::string> globalsSet_;

  std::vector<LoopFrame> loopStack_;
  std::vector<int> withStack_;

  std::vector<std::string> imported_;
  std::vector<std::string> extensions_;
  std::vector<std::string> usedBuiltins_;
  std::vector<Constant> consts_;

  bool inMethod_ = false;
  bool failed_ = false;
};

}  // namespace pcc
