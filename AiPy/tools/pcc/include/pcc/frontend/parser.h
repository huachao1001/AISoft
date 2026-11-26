// 递归下降解析器：记号流 → AST。
//
// ── 为什么是递归下降 ──
//
// 目标语法是 Python 的一个**确定子集**（见 LANGUAGE.md），不是完整语法。
// 子集里没有 yield、async、match、推导式嵌套，递归下降最短、错误最可控。
// CPython 用 PEG 是因为它要覆盖整个语言并支持 `:=`、`/` 分隔符这类上下文
// 相关规则；我们不需要那个代价。
//
// ── 原则：能识别、但要拒绝 ──
//
// 不支持的东西（元组、集合、f-string、lambda…）**必须在语法层识别出来**，
// 然后由代码生成层给出明确提示。解析器直接报「语法错误」会让用户不知道
// 问题在哪。所以这里把那些形态都解析成节点，交给下层拒绝。
#pragma once

#include "pcc/ast.h"
#include "pcc/frontend/token.h"

#include <string>
#include <vector>

namespace pcc {

struct ParseError {
  std::string message;
  int line = 1;
  int col = 0;
};

class Parser {
 public:
  // tokens 必须已成功词法分析（末尾有 End）。
  Parser(const std::vector<Token>& tokens, std::string filename);

  // 解析整个模块。失败返回空指针，原因在 error()。
  NodePtr parseModule();

  const ParseError& error() const { return error_; }

 private:
  // ── 记号操作 ──
  const Token& peek(std::size_t ahead = 0) const;
  bool check(TokenType type) const;
  bool checkOp(const char* text) const;
  bool checkName(const char* text) const;
  const Token& advance();
  bool matchOp(const char* text);
  bool matchName(const char* text);
  bool expectOp(const char* text);
  bool expectNewline();
  void skipNewlines();
  bool fail(const std::string& message);

  // ── 语句（statements.cpp）──
  NodePtr statement();
  NodePtr decorated();
  NodePtr simpleStatement();
  NodePtr smallStatement();
  NodePtr compoundStatement();
  NodePtr ifStatement();
  NodePtr whileStatement();
  NodePtr forStatement();
  NodePtr functionDef(bool async);
  NodePtr classDef();
  NodePtr withStatement(bool async);
  NodePtr tryStatement();
  NodePtr raiseStatement();
  NodePtr matchStatement();
  NodePtr parseFunctionArgs();
  bool parseBlock(std::vector<NodePtr>* body);
  NodePtr importStatement();
  NodePtr fromImportStatement();
  NodePtr returnStatement();
  NodePtr globalStatement(bool nonlocal);
  NodePtr rejectRemainder(Kind kind);  // 识别并跳过（del/assert/raise...）

  // ── 表达式（expressions.cpp）──
  NodePtr expression();
  NodePtr expressionList();
  NodePtr targetList();
  NodePtr orExpr();
  NodePtr andExpr();
  NodePtr notExpr();
  NodePtr comparison();
  NodePtr bitOr();
  NodePtr bitXor();
  NodePtr bitAnd();
  NodePtr shift();
  NodePtr arith();
  NodePtr term();
  NodePtr factor();
  NodePtr power();
  NodePtr atomExpr();
  NodePtr atom();
  NodePtr trailer(NodePtr base);
  NodePtr parseAtomParen();
  NodePtr parseAtomList();
  NodePtr parseAtomBrace();
  bool parseCallArgs(Node* call);
  NodePtr parseSubscriptList();
  NodePtr parseSliceItem();

  const std::vector<Token>& tokens_;
  std::string filename_;
  std::size_t at_ = 0;
  ParseError error_;
  bool failed_ = false;
};

}  // namespace pcc
