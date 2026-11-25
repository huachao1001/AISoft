// 解析器的骨架：记号操作与模块入口。
//
// 语句与表达式分在 statements.cpp / expressions.cpp 两个文件里——这两块
// 各近八百行，混在一起读的人是找不到边界的。
#include "pcc/frontend/parser.h"

#include "pcc/diagnostics.h"

namespace pcc {

Parser::Parser(const std::vector<Token>& tokens, std::string filename)
    : tokens_(tokens), filename_(std::move(filename)) {}

const Token& Parser::peek(std::size_t ahead) const {
  const std::size_t at = at_ + ahead;
  if (at < tokens_.size()) return tokens_[at];
  return tokens_.back();
}

bool Parser::check(TokenType type) const { return peek().type == type; }

bool Parser::checkOp(const char* text) const {
  return peek().type == TokenType::Op && peek().text == text;
}

bool Parser::checkName(const char* text) const {
  return peek().type == TokenType::Name && peek().text == text;
}

const Token& Parser::advance() {
  const Token& token = peek();
  if (at_ + 1 < tokens_.size()) ++at_;
  return token;
}

bool Parser::matchOp(const char* text) {
  if (!checkOp(text)) return false;
  advance();
  return true;
}

bool Parser::matchName(const char* text) {
  if (!checkName(text)) return false;
  advance();
  return true;
}

bool Parser::expectOp(const char* text) {
  if (matchOp(text)) return true;
  return fail(diag::expectedToken(text, peek().text));
}

bool Parser::expectNewline() {
  if (check(TokenType::Newline)) {
    advance();
    return true;
  }
  if (check(TokenType::End)) return true;
  return fail(diag::expectedNewline());
}

void Parser::skipNewlines() {
  while (check(TokenType::Newline)) advance();
}

bool Parser::fail(const std::string& message) {
  if (failed_) return false;
  failed_ = true;
  error_.message = message;
  error_.line = peek().line;
  error_.col = peek().col;
  return false;
}

NodePtr Parser::parseModule() {
  auto module = make_node(Kind::Module, 1, 0);
  skipNewlines();
  while (!check(TokenType::End) && !failed_) {
    NodePtr node = statement();
    if (!node) return nullptr;
    if (node->kind == Kind::StatementList) {
      for (auto& child : node->children) module->children.push_back(std::move(child));
    } else {
      module->children.push_back(std::move(node));
    }
    skipNewlines();
  }
  if (failed_) return nullptr;
  return module;
}

}  // namespace pcc
