// 表达式解析。
//
// ── 优先级表（低 → 高），与 CPython 的 grammar 一致 ──
//
//   expression   : orExpr ['if' orExpr 'else' expression]
//   orExpr       : andExpr ('or' andExpr)*
//   andExpr      : notExpr ('and' notExpr)*
//   notExpr      : 'not' notExpr | comparison
//   comparison   : bitOr (compOp bitOr)*
//   bitOr        : bitXor ('|' bitXor)*
//   bitXor       : bitAnd ('^' bitAnd)*
//   bitAnd       : shift ('&' shift)*
//   shift        : arith (('<<'|'>>') arith)*
//   arith        : term (('+'|'-') term)*
//   term         : factor (('*'|'/'|'//'|'%'|'@') factor)*
//   factor       : ('+'|'-'|'~') factor | power
//   power        : atomExpr ['**' factor]
//   atomExpr     : atom trailer*
//
// `**` 右结合、且比一元 `-` 绑得紧（`-2**2 == -4`），所以 power 在 factor
// 下面单独一层。这一条最容易写错，而写错不会报错，只是数值不对。
#include "pcc/frontend/parser.h"

#include "pcc/diagnostics.h"

namespace pcc {
namespace {

Kind binaryKind(const std::string& text) {
  if (text == "|") return Kind::BitOr;
  if (text == "^") return Kind::BitXor;
  if (text == "&") return Kind::BitAnd;
  if (text == "<<") return Kind::LShift;
  if (text == ">>") return Kind::RShift;
  if (text == "+") return Kind::Add;
  if (text == "-") return Kind::Sub;
  if (text == "*") return Kind::Mult;
  if (text == "/") return Kind::Div;
  if (text == "//") return Kind::FloorDiv;
  if (text == "%") return Kind::Mod;
  return Kind::MatMult;
}

}  // namespace

NodePtr Parser::expression() {
  NodePtr body = orExpr();
  if (!body) return nullptr;
  // 海象 `:=`：识别出来交给 codegen 拒绝（NamedExpr）。不识别的话，
  // `(n := 5) > 0` 会在 `)` 处报「expected ')'」，用户看不出去掉它就行。
  if (checkOp(":=")) {
    const Token& token = peek();
    advance();
    NodePtr value = expression();
    if (!value) return nullptr;
    auto node = make_node(Kind::NamedExpr, token.line, token.col);
    node->children.push_back(std::move(body));
    node->children.push_back(std::move(value));
    return node;
  }
  if (checkName("if")) {
    const Token& token = peek();
    advance();
    NodePtr test = orExpr();
    if (!test) return nullptr;
    if (!matchName("else")) {
      fail(diag::expectedToken("else", peek().text));
      return nullptr;
    }
    NodePtr orelse = expression();
    if (!orelse) return nullptr;
    auto node = make_node(Kind::IfExp, token.line, token.col);
    node->children.push_back(std::move(test));
    node->children.push_back(std::move(body));
    node->children.push_back(std::move(orelse));
    return node;
  }
  return body;
}

NodePtr Parser::expressionList() {
  // 逗号连接的一串表达式：`a, b = 1, 2` 的右侧、`return a, b` 等。
  NodePtr first = expression();
  if (!first) return nullptr;
  if (!checkOp(",")) return first;

  auto tuple = make_node(Kind::TupleLit, first->line, first->col);
  tuple->children.push_back(std::move(first));
  while (matchOp(",")) {
    // 尾随逗号：`(1,)` / `a, = x`
    if (check(TokenType::Newline) || check(TokenType::End) || checkOp("=") ||
        checkOp(")") || checkOp("]") || checkOp("}") || checkOp(":")) {
      break;
    }
    NodePtr item = expression();
    if (!item) return nullptr;
    tuple->children.push_back(std::move(item));
  }
  return tuple;
}

NodePtr Parser::targetList() {
  // for 的循环目标：在 bitOr 层解析，**不进入比较层**——比较层会吃掉 `in`，
  // 把 `for i in xs` 的循环变量解析成一个布尔表达式，然后在 `:` 处报
  // 「missing 'in'」。Python 的 star_targets 同样不含比较。
  NodePtr first = bitOr();
  if (!first) return nullptr;
  if (!checkOp(",")) return first;

  auto tuple = make_node(Kind::TupleLit, first->line, first->col);
  tuple->children.push_back(std::move(first));
  while (matchOp(",")) {
    if (checkName("in")) break;
    NodePtr item = bitOr();
    if (!item) return nullptr;
    tuple->children.push_back(std::move(item));
  }
  return tuple;
}

NodePtr Parser::orExpr() {
  NodePtr left = andExpr();
  if (!left) return nullptr;
  if (!checkName("or")) return left;

  auto node = make_node(Kind::BoolOp, left->line, left->col);
  node->ops.push_back(Kind::Or);
  node->opLines.push_back(left->line);
  node->children.push_back(std::move(left));
  while (matchName("or")) {
    NodePtr right = andExpr();
    if (!right) return nullptr;
    node->children.push_back(std::move(right));
  }
  return node;
}

NodePtr Parser::andExpr() {
  NodePtr left = notExpr();
  if (!left) return nullptr;
  if (!checkName("and")) return left;

  auto node = make_node(Kind::BoolOp, left->line, left->col);
  node->ops.push_back(Kind::And);
  node->opLines.push_back(left->line);
  node->children.push_back(std::move(left));
  while (matchName("and")) {
    NodePtr right = notExpr();
    if (!right) return nullptr;
    node->children.push_back(std::move(right));
  }
  return node;
}

NodePtr Parser::notExpr() {
  if (checkName("not")) {
    const Token& token = peek();
    advance();
    NodePtr operand = notExpr();
    if (!operand) return nullptr;
    auto node = make_node(Kind::UnaryOp, token.line, token.col);
    node->ops.push_back(Kind::Not);
    node->opLines.push_back(token.line);
    node->children.push_back(std::move(operand));
    return node;
  }
  return comparison();
}

NodePtr Parser::comparison() {
  NodePtr left = bitOr();
  if (!left) return nullptr;

  auto node = make_node(Kind::Compare, left->line, left->col);
  node->children.push_back(std::move(left));

  for (;;) {
    Kind op = Kind::Eq;
    bool matched = true;
    if (checkOp("==")) op = Kind::Eq;
    else if (checkOp("!=")) op = Kind::NotEq;
    else if (checkOp("<")) op = Kind::Lt;
    else if (checkOp("<=")) op = Kind::LtE;
    else if (checkOp(">")) op = Kind::Gt;
    else if (checkOp(">=")) op = Kind::GtE;
    else if (checkName("in")) op = Kind::In;
    else if (checkName("not") && peek(1).type == TokenType::Name &&
             peek(1).text == "in") op = Kind::NotIn;
    else if (checkName("is") && peek(1).type == TokenType::Name &&
             peek(1).text == "not") op = Kind::IsNot;
    else if (checkName("is")) op = Kind::Is;
    else matched = false;

    if (!matched) break;

    const int opLine = peek().line;
    if (op == Kind::NotIn || op == Kind::IsNot) {
      advance();
      advance();
    } else {
      advance();
    }

    NodePtr right = bitOr();
    if (!right) return nullptr;
    node->ops.push_back(op);
    node->opLines.push_back(opLine);
    node->children.push_back(std::move(right));
  }

  if (node->ops.empty()) return std::move(node->children[0]);
  return node;
}

NodePtr Parser::bitOr() {
  NodePtr left = bitXor();
  if (!left) return nullptr;
  while (checkOp("|")) {
    const Token& token = peek();
    advance();
    NodePtr right = bitXor();
    if (!right) return nullptr;
    auto node = make_node(Kind::BinOp, left->line, left->col);
    node->ops.push_back(Kind::BitOr);
    node->opLines.push_back(token.line);
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
    left = std::move(node);
  }
  return left;
}

NodePtr Parser::bitXor() {
  NodePtr left = bitAnd();
  if (!left) return nullptr;
  while (checkOp("^")) {
    const Token& token = peek();
    advance();
    NodePtr right = bitAnd();
    if (!right) return nullptr;
    auto node = make_node(Kind::BinOp, left->line, left->col);
    node->ops.push_back(Kind::BitXor);
    node->opLines.push_back(token.line);
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
    left = std::move(node);
  }
  return left;
}

NodePtr Parser::bitAnd() {
  NodePtr left = shift();
  if (!left) return nullptr;
  while (checkOp("&")) {
    const Token& token = peek();
    advance();
    NodePtr right = shift();
    if (!right) return nullptr;
    auto node = make_node(Kind::BinOp, left->line, left->col);
    node->ops.push_back(Kind::BitAnd);
    node->opLines.push_back(token.line);
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
    left = std::move(node);
  }
  return left;
}

NodePtr Parser::shift() {
  NodePtr left = arith();
  if (!left) return nullptr;
  while (checkOp("<<") || checkOp(">>")) {
    const Token& token = peek();
    const Kind op = binaryKind(token.text);
    advance();
    NodePtr right = arith();
    if (!right) return nullptr;
    auto node = make_node(Kind::BinOp, left->line, left->col);
    node->ops.push_back(op);
    node->opLines.push_back(token.line);
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
    left = std::move(node);
  }
  return left;
}

NodePtr Parser::arith() {
  NodePtr left = term();
  if (!left) return nullptr;
  while (checkOp("+") || checkOp("-")) {
    const Token& token = peek();
    const Kind op = binaryKind(token.text);
    advance();
    NodePtr right = term();
    if (!right) return nullptr;
    auto node = make_node(Kind::BinOp, left->line, left->col);
    node->ops.push_back(op);
    node->opLines.push_back(token.line);
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
    left = std::move(node);
  }
  return left;
}

NodePtr Parser::term() {
  NodePtr left = factor();
  if (!left) return nullptr;
  for (;;) {
    if (!checkOp("*") && !checkOp("/") && !checkOp("//") && !checkOp("%") &&
        !checkOp("@")) {
      break;
    }
    const Token& token = peek();
    const Kind op = binaryKind(token.text);
    advance();
    NodePtr right = factor();
    if (!right) return nullptr;
    auto node = make_node(Kind::BinOp, left->line, left->col);
    node->ops.push_back(op);
    node->opLines.push_back(token.line);
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
    left = std::move(node);
  }
  return left;
}

NodePtr Parser::factor() {
  if (checkOp("-") || checkOp("+") || checkOp("~")) {
    const Token& token = peek();
    const Kind op = token.text == "-" ? Kind::USub
                    : token.text == "+" ? Kind::UAdd
                                        : Kind::Invert;
    advance();
    NodePtr operand = factor();
    if (!operand) return nullptr;
    auto node = make_node(Kind::UnaryOp, token.line, token.col);
    node->ops.push_back(op);
    node->opLines.push_back(token.line);
    node->children.push_back(std::move(operand));
    return node;
  }
  return power();
}

NodePtr Parser::power() {
  NodePtr base = atomExpr();
  if (!base) return nullptr;
  if (checkOp("**")) {
    const Token& token = peek();
    advance();
    // 右结合：`2**3**2 == 2**(3**2)`
    NodePtr exponent = factor();
    if (!exponent) return nullptr;
    auto node = make_node(Kind::BinOp, base->line, base->col);
    node->ops.push_back(Kind::Pow);
    node->opLines.push_back(token.line);
    node->children.push_back(std::move(base));
    node->children.push_back(std::move(exponent));
    return node;
  }
  return base;
}

NodePtr Parser::atomExpr() {
  NodePtr base = atom();
  if (!base) return nullptr;
  for (;;) {
    if (checkOp("(") || checkOp("[") || checkOp(".")) {
      base = trailer(std::move(base));
      if (!base) return nullptr;
      continue;
    }
    break;
  }
  return base;
}

NodePtr Parser::trailer(NodePtr base) {
  if (checkOp("(")) {
    auto node = make_node(Kind::Call, base->line, base->col);
    node->children.push_back(std::move(base));
    advance();
    if (!parseCallArgs(node.get())) return nullptr;
    return node;
  }
  if (checkOp("[")) {
    advance();
    auto node = make_node(Kind::Subscript, base->line, base->col);
    node->children.push_back(std::move(base));
    NodePtr slice = parseSubscriptList();
    if (!slice) return nullptr;
    node->children.push_back(std::move(slice));
    if (!expectOp("]")) return nullptr;
    return node;
  }
  if (matchOp(".")) {
    auto node = make_node(Kind::Attribute, base->line, base->col);
    if (!check(TokenType::Name)) {
      fail(diag::nameExpected("'.'"));
      return nullptr;
    }
    node->text = advance().text;
    node->selfAttr = base->kind == Kind::Name && base->text == "self";
    node->children.push_back(std::move(base));
    return node;
  }
  fail(diag::strayToken(peek().text));
  return nullptr;
}

bool Parser::parseCallArgs(Node* call) {
  std::size_t positionals = 0;
  while (!checkOp(")") && !check(TokenType::End)) {
    if (checkOp("*") || checkOp("**")) {
      const Token& token = peek();
      advance();
      auto starred = make_node(Kind::Starred, token.line, token.col);
      if (token.text == "**") starred->flag = true;
      NodePtr value = expression();
      if (!value) return false;
      starred->children.push_back(std::move(value));
      call->children.push_back(std::move(starred));
      ++positionals;
      if (!matchOp(",")) break;
      continue;
    }
    // 关键字参数 name=value
    if (check(TokenType::Name) && peek(1).type == TokenType::Op &&
        peek(1).text == "=") {
      const Token& nameToken = peek();
      advance();
      advance();
      NodePtr value = expression();
      if (!value) return false;
      auto keyword = make_node(Kind::Keyword, nameToken.line, nameToken.col);
      keyword->text = nameToken.text;
      keyword->children.push_back(std::move(value));
      call->children.push_back(std::move(keyword));
      if (!matchOp(",")) break;
      continue;
    }
    NodePtr value = expression();
    if (!value) return false;
    call->children.push_back(std::move(value));
    ++positionals;
    if (!matchOp(",")) break;
  }
  call->argsCount = positionals;
  return expectOp(")");
}

NodePtr Parser::parseSubscriptList() {
  NodePtr first = parseSliceItem();
  if (!first) return nullptr;
  if (!checkOp(",")) return first;

  // `a[1, 2]`：元组下标。不支持，但识别出来给准确提示。
  auto tuple = make_node(Kind::TupleLit, first->line, first->col);
  tuple->children.push_back(std::move(first));
  while (matchOp(",")) {
    if (checkOp("]")) break;
    NodePtr item = parseSliceItem();
    if (!item) return nullptr;
    tuple->children.push_back(std::move(item));
  }
  return tuple;
}

NodePtr Parser::parseSliceItem() {
  NodePtr start;
  if (!checkOp(":")) {
    start = expression();
    if (!start) return nullptr;
    if (!checkOp(":")) return start;
  }

  auto slice = make_node(Kind::Slice, peek().line, peek().col);
  if (!expectOp(":")) return nullptr;
  if (start) {
    slice->sliceFlags |= 1;
    slice->children.push_back(std::move(start));
  }
  if (!checkOp(":") && !checkOp("]") && !checkOp(",") && !check(TokenType::End)) {
    NodePtr stop = expression();
    if (!stop) return nullptr;
    slice->sliceFlags |= 2;
    slice->children.push_back(std::move(stop));
  }
  if (matchOp(":")) {
    if (!checkOp("]") && !checkOp(",") && !check(TokenType::End)) {
      NodePtr step = expression();
      if (!step) return nullptr;
      slice->sliceFlags |= 4;
      slice->children.push_back(std::move(step));
    }
  }
  return slice;
}

NodePtr Parser::atom() {
  const Token& token = peek();

  if (token.type == TokenType::Number) {
    advance();
    auto node = make_node(Kind::Constant, token.line, token.col);
    node->text = token.raw;
    node->constKind =
        (token.isFloat || token.isComplex) ? ConstKind::Float : ConstKind::Int;
    node->flag = token.isComplex;  // 复数要在 codegen 里拒绝
    return node;
  }

  if (token.type == TokenType::String) {
    advance();
    if (token.isFString) {
      // f-string：内容要当表达式编译，而 LANGUAGE.md 明确不支持。
      auto joined = make_node(Kind::JoinedStr, token.line, token.col);
      joined->text = token.text;
      return joined;
    }

    auto node = make_node(Kind::Constant, token.line, token.col);
    node->text = token.text;
    node->constKind = ConstKind::Str;
    node->flag = token.isBytes;

    // 相邻字符串自动拼接：`"a" "b"` → `"ab"`
    while (check(TokenType::String) && !check(TokenType::End)) {
      const Token& next = advance();
      node->text += next.text;
      if (next.isBytes) node->flag = true;
    }
    return node;
  }

  if (token.type == TokenType::Name) {
    if (token.text == "True" || token.text == "False") {
      advance();
      auto node = make_node(Kind::Constant, token.line, token.col);
      node->constKind = ConstKind::Bool;
      node->intValue = token.text == "True" ? 1 : 0;
      return node;
    }
    if (token.text == "None") {
      advance();
      auto node = make_node(Kind::Constant, token.line, token.col);
      node->constKind = ConstKind::None;
      return node;
    }
    if (token.text == "lambda") {
      advance();
      auto node = make_node(Kind::Lambda, token.line, token.col);
      while (!checkOp(":") && !check(TokenType::End)) advance();
      matchOp(":");
      NodePtr body = expression();
      if (body) node->children.push_back(std::move(body));
      return node;
    }
    if (token.text == "await") {
      advance();
      auto node = make_node(Kind::Await, token.line, token.col);
      NodePtr value = expression();
      if (value) node->children.push_back(std::move(value));
      return node;
    }
    if (token.text == "yield") {
      advance();
      auto node = make_node(Kind::Yield, token.line, token.col);
      if (!check(TokenType::Newline) && !checkOp(")") && !checkOp("]") &&
          !checkOp(",") && !check(TokenType::End)) {
        NodePtr value = expression();
        if (value) node->children.push_back(std::move(value));
      }
      return node;
    }

    advance();
    auto node = make_node(Kind::Name, token.line, token.col);
    node->text = token.text;
    return node;
  }

  if (token.type == TokenType::Op) {
    if (token.text == "..." || token.isEllipsis) {
      advance();
      auto node = make_node(Kind::Constant, token.line, token.col);
      node->text = "Ellipsis";
      node->constKind = ConstKind::None;
      node->intValue = -1;  // 区分 Ellipsis 与 None
      return node;
    }
    if (token.text == "(") return parseAtomParen();
    if (token.text == "[") return parseAtomList();
    if (token.text == "{") return parseAtomBrace();
    if (token.text == "*") {
      advance();
      auto node = make_node(Kind::Starred, token.line, token.col);
      NodePtr value = expression();
      if (value) node->children.push_back(std::move(value));
      return node;
    }
  }

  fail(diag::unsupportedExpressionToken(token.text));
  return nullptr;
}

NodePtr Parser::parseAtomParen() {
  const Token& token = peek();
  advance();  // '('
  if (checkOp(")")) {
    advance();
    return make_node(Kind::TupleLit, token.line, token.col);  // 空元组
  }
  NodePtr first = expression();
  if (!first) return nullptr;
  if (checkOp(",")) {
    auto node = make_node(Kind::TupleLit, token.line, token.col);
    node->children.push_back(std::move(first));
    while (matchOp(",")) {
      if (checkOp(")")) break;
      NodePtr item = expression();
      if (!item) return nullptr;
      node->children.push_back(std::move(item));
    }
    if (!expectOp(")")) return nullptr;
    return node;
  }
  if (!expectOp(")")) return nullptr;
  return first;  // 括号只是分组
}

NodePtr Parser::parseAtomList() {
  const Token& token = peek();
  advance();  // '['
  auto node = make_node(Kind::ListLit, token.line, token.col);
  while (!checkOp("]") && !check(TokenType::End)) {
    NodePtr item = expression();
    if (!item) return nullptr;
    // 列表推导式：识别出来给准确提示（内容不重要，反正要拒绝）。
    if (checkName("for")) {
      auto comp = make_node(Kind::ListComp, token.line, token.col);
      while (!checkOp("]") && !check(TokenType::End)) advance();
      if (!expectOp("]")) return nullptr;
      return comp;
    }
    node->children.push_back(std::move(item));
    if (!matchOp(",")) break;
  }
  if (!expectOp("]")) return nullptr;
  return node;
}

NodePtr Parser::parseAtomBrace() {
  const Token& token = peek();
  advance();  // '{'
  if (checkOp("}")) {
    advance();
    return make_node(Kind::DictLit, token.line, token.col);
  }

  // `{**d}` 与 `{**d, "a": 1}`：展开一律拒绝。用「子节点数 ≠ 键数×2」
  // 传递这个事实，codegen 照 Python 版的文案报。
  if (checkOp("**")) {
    auto node = make_node(Kind::DictLit, token.line, token.col);
    for (;;) {
      if (matchOp("**")) {
        NodePtr value = expression();
        if (!value) return nullptr;
        node->children.push_back(std::move(value));
      } else {
        NodePtr key = expression();
        if (!key) return nullptr;
        if (!expectOp(":")) return nullptr;
        NodePtr value = expression();
        if (!value) return nullptr;
        node->children.push_back(std::move(key));
        node->children.push_back(std::move(value));
        ++node->keyCount;
      }
      if (!matchOp(",")) break;
      if (checkOp("}")) break;
    }
    if (!expectOp("}")) return nullptr;
    return node;
  }

  NodePtr first = expression();
  if (!first) return nullptr;
  if (checkOp(":")) {
    auto node = make_node(Kind::DictLit, token.line, token.col);
    advance();
    NodePtr value = expression();
    if (!value) return nullptr;
    node->children.push_back(std::move(first));
    node->children.push_back(std::move(value));
    ++node->keyCount;
    while (matchOp(",")) {
      if (checkOp("}")) break;
      NodePtr key = expression();
      if (!key) return nullptr;
      if (!expectOp(":")) return nullptr;
      NodePtr item = expression();
      if (!item) return nullptr;
      node->children.push_back(std::move(key));
      node->children.push_back(std::move(item));
      ++node->keyCount;
    }
    if (!expectOp("}")) return nullptr;
    return node;
  }
  // 集合字面量：识别出来给准确提示
  auto node = make_node(Kind::SetLit, token.line, token.col);
  node->children.push_back(std::move(first));
  while (matchOp(",")) {
    if (checkOp("}")) break;
    NodePtr item = expression();
    if (!item) return nullptr;
    node->children.push_back(std::move(item));
  }
  if (!expectOp("}")) return nullptr;
  return node;
}

}  // namespace pcc
