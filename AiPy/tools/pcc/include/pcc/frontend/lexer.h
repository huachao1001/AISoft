// 词法分析：源码 → 记号流。
//
// ── 为什么手写而不是用正则 ──
//
// Python 的词法有相当多「上下文相关」的部分：缩进决定块结构、括号内可以
// 随意换行、字符串前缀改变转义规则、反斜杠续行。正则能切单词，但切不出
// INDENT/DEDENT，而那正是块结构的全部信息。
//
// ── 与 CPython 的差异 ──
//
// 只覆盖 LANGUAGE.md 定义的那份子集，且刻意**多认一些**语法：像元组、
// 集合、f-string 这些「不支持」的东西，词法与语法都要能识别出来，才能在
// 编译期给出准确提示。识别不了就只能报「语法错误」，用户看不出哪里不该用。
#pragma once

#include "pcc/frontend/token.h"

#include <string>
#include <vector>

namespace pcc {

struct LexError {
  std::string message;
  int line = 1;
  int col = 0;
};

class Lexer {
 public:
  // lineOffset 用于 f-string 这类「从源码中间切出一段再单独词法分析」的场景，
  // 让报错与字节码行号仍指向原始文件的那一行，而不是恒从 1 开始。
  Lexer(std::string source, std::string filename, int startLine = 1);

  // 切出全部记号。失败返回 false，原因在 error()。
  bool tokenize();

  const std::vector<Token>& tokens() const { return tokens_; }
  const LexError& error() const { return error_; }

 private:
  char peek(std::size_t ahead = 0) const;
  bool atEnd() const { return pos_ >= source_.size(); }
  void advance();

  bool handleLineStart();
  bool scanName();
  bool scanNumber();
  bool scanStringFromBody(Token* token, bool raw);
  bool scanOperator();
  void skipSpacesAndComment();  void emit(TokenType type, std::string text, int line, int col);
  void fail(const std::string& message);

  int depth_ = 0;  // 括号深度：>0 时换行不算逻辑行结束

  std::string source_;
  std::string filename_;
  std::size_t pos_ = 0;
  int line_ = 1;
  int col_ = 0;

  std::vector<int> indents_{0};
  std::vector<Token> tokens_;
  LexError error_;
  bool failed_ = false;
};

}  // namespace pcc
