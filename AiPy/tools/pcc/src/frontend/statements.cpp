// 语句解析。
//
// ── 块结构怎么表达 ──
//
// 复合语句的子句体放在 children 里，用 bodyCount / orelseCount 切分：
//
//   If:   [test] + body(bodyCount) + orelse(orelseCount)
//   While/For: [test/target, iter] + body + orelse
//   def:  [Arguments] + body(bodyCount)
//
// 这样切分而不是用单独的 vector<NodePtr> 成员，是因为 codegen 要按源码
// 顺序遍历——多一层容器就多一层搬家。
#include "pcc/frontend/parser.h"

#include "pcc/diagnostics.h"

#include <cstdlib>

namespace pcc {
namespace {

bool isAugAssignOp(const std::string& text) {
  return text == "+=" || text == "-=" || text == "*=" || text == "/=" ||
         text == "//=" || text == "%=" || text == "**=" || text == "&=" ||
         text == "|=" || text == "^=" || text == "<<=" || text == ">>=" ||
         text == "@=";
}

// `+=` → `Add`：去掉末位 `=` 再对照。
Kind augToBinary(const std::string& text) {
  const std::string base = text.substr(0, text.size() - 1);
  if (base == "+") return Kind::Add;
  if (base == "-") return Kind::Sub;
  if (base == "*") return Kind::Mult;
  if (base == "/") return Kind::Div;
  if (base == "//") return Kind::FloorDiv;
  if (base == "%") return Kind::Mod;
  if (base == "**") return Kind::Pow;
  if (base == "&") return Kind::BitAnd;
  if (base == "|") return Kind::BitOr;
  if (base == "^") return Kind::BitXor;
  if (base == "<<") return Kind::LShift;
  if (base == ">>") return Kind::RShift;
  return Kind::MatMult;
}

}  // namespace

NodePtr Parser::statement() {
  if (check(TokenType::Indent)) {
    fail(diag::unexpectedIndent());
    return nullptr;
  }
  if (checkOp("@")) return decorated();
  if (checkName("if") || checkName("while") || checkName("for") ||
      checkName("def") || checkName("class") || checkName("with") ||
      checkName("async") || checkName("try") || checkName("match")) {
    return compoundStatement();
  }
  return simpleStatement();
}

NodePtr Parser::decorated() {
  // 装饰器一律拒绝，但必须**解析**掉：否则 `@dec\ndef f():` 会报
  // 「unexpected token '@'」，用户看不出真正的限制在哪。
  // 装饰器表达式的求值顺序、嵌套层数都不重要，只要认出有几个、位置在哪。
  int count = 0;
  int line = peek().line;
  int col = peek().col;
  std::vector<NodePtr> decorators;
  while (checkOp("@")) {
    advance();
    NodePtr decorator = expression();
    if (!decorator) return nullptr;
    if (count == 0) {
      line = decorator->line;
      col = decorator->col;
    }
    decorators.push_back(std::move(decorator));
    ++count;
    if (!expectNewline()) return nullptr;
    skipNewlines();
  }

  NodePtr node = statement();
  if (!node) return nullptr;
  if (node->kind == Kind::FunctionDef || node->kind == Kind::ClassDef ||
      node->kind == Kind::AsyncFunctionDef) {
    node->decoratorCount = static_cast<std::size_t>(count);
    node->decoratorLine = line;
    node->decoratorCol = col;
    node->decorators = std::move(decorators);
    return node;
  }
  fail(diag::strayToken("@"));
  return nullptr;
}

NodePtr Parser::simpleStatement() {
  // 一行多条语句：`a = 1; b = 2`。
  auto line = make_node(Kind::StatementList, peek().line, peek().col);
  for (;;) {
    NodePtr node = smallStatement();
    if (!node) return nullptr;
    line->children.push_back(std::move(node));
    if (!matchOp(";")) break;
    if (check(TokenType::Newline) || check(TokenType::End)) break;
  }
  if (!expectNewline()) return nullptr;
  if (line->children.size() == 1) return std::move(line->children[0]);
  return line;
}

NodePtr Parser::rejectRemainder(Kind kind) {
  // del / assert / raise：只要识别出种类就够，内容整段跳过。
  auto node = make_node(kind, peek().line, peek().col);
  advance();
  while (!check(TokenType::Newline) && !check(TokenType::End)) advance();
  return node;
}

NodePtr Parser::deleteStatement() {
  auto node = make_node(Kind::Delete, peek().line, peek().col);
  advance();  // del
  // 只支持 `del 名字` 与 `del 名字, 名字`。下标/属性删除需要 VM 的
  // DICT_DELETE 之类语义，留给后面按需加。
  for (;;) {
    if (!check(TokenType::Name)) {
      fail(diag::nameExpected("a variable name"));
      return nullptr;
    }
    NodePtr target = make_node(Kind::Name, peek().line, peek().col);
    target->text = peek().text;
    advance();
    node->children.push_back(std::move(target));
    if (!matchOp(",")) break;
  }
  return node;
}

NodePtr Parser::assertStatement() {
  auto node = make_node(Kind::Assert, peek().line, peek().col);
  advance();  // assert
  NodePtr condition = expression();
  if (condition == nullptr) return nullptr;
  node->children.push_back(std::move(condition));
  // `assert cond, "message"`：消息是可选的，逗号后接任意表达式。
  if (matchOp(",")) {
    NodePtr message = expression();
    if (message == nullptr) return nullptr;
    node->children.push_back(std::move(message));
  }
  return node;
}

NodePtr Parser::returnStatement() {
  auto node = make_node(Kind::Return, peek().line, peek().col);
  advance();
  if (!check(TokenType::Newline) && !checkOp(";") && !check(TokenType::End)) {
    NodePtr value = expressionList();
    if (!value) return nullptr;
    node->children.push_back(std::move(value));
  }
  return node;
}

NodePtr Parser::globalStatement(bool nonlocal) {
  auto node = make_node(nonlocal ? Kind::Nonlocal : Kind::Global, peek().line,
                        peek().col);
  advance();
  for (;;) {
    if (!check(TokenType::Name)) {
      fail(diag::nameExpected(nonlocal ? "nonlocal" : "global"));
      return nullptr;
    }
    node->names.push_back(advance().text);
    if (!matchOp(",")) break;
  }
  return node;
}

NodePtr Parser::importStatement() {
  auto node = make_node(Kind::Import, peek().line, peek().col);
  advance();  // 'import'
  for (;;) {
    if (!check(TokenType::Name)) {
      fail(diag::nameExpected("import"));
      return nullptr;
    }
    auto alias = make_node(Kind::Alias, peek().line, peek().col);
    alias->text = advance().text;
    // 点分模块名
    while (matchOp(".")) {
      if (!check(TokenType::Name)) {
        fail(diag::nameExpected("'.'"));
        return nullptr;
      }
      alias->text += "." + advance().text;
    }
    if (matchName("as")) {
      if (!check(TokenType::Name)) {
        fail(diag::nameExpected("'as'"));
        return nullptr;
      }
      alias->raw = advance().text;
    }
    node->children.push_back(std::move(alias));
    if (!matchOp(",")) break;
  }
  return node;
}

NodePtr Parser::fromImportStatement() {
  auto node = make_node(Kind::ImportFrom, peek().line, peek().col);
  advance();  // 'from'
  int dots = 0;
  while (matchOp(".")) ++dots;
  node->flag = dots > 0;

  if (check(TokenType::Name)) {
    node->text = advance().text;
    while (matchOp(".")) {
      if (!check(TokenType::Name)) break;
      node->text += "." + advance().text;
    }
  }
  if (!matchName("import")) {
    fail(diag::nameExpected("'from ... import'"));
    return nullptr;
  }
  if (checkOp("*")) {
    advance();
    auto alias = make_node(Kind::Alias, peek().line, peek().col);
    alias->text = "*";
    node->children.push_back(std::move(alias));
    return node;
  }
  for (;;) {
    if (!check(TokenType::Name)) {
      fail(diag::nameExpected("import"));
      return nullptr;
    }
    auto alias = make_node(Kind::Alias, peek().line, peek().col);
    alias->text = advance().text;
    if (matchName("as")) {
      if (!check(TokenType::Name)) {
        fail(diag::nameExpected("'as'"));
        return nullptr;
      }
      alias->raw = advance().text;
    }
    node->children.push_back(std::move(alias));
    if (!matchOp(",")) break;
  }
  return node;
}

NodePtr Parser::smallStatement() {
  const Token& token = peek();

  if (checkName("pass")) {
    advance();
    return make_node(Kind::Pass, token.line, token.col);
  }
  if (checkName("break")) {
    advance();
    return make_node(Kind::Break, token.line, token.col);
  }
  if (checkName("continue")) {
    advance();
    return make_node(Kind::Continue, token.line, token.col);
  }
  if (checkName("return")) return returnStatement();
  if (checkName("global")) return globalStatement(false);
  if (checkName("nonlocal")) return globalStatement(true);
  if (checkName("del")) return deleteStatement();
  if (checkName("assert")) return assertStatement();
  if (checkName("raise")) return raiseStatement();
  if (checkName("import")) return importStatement();
  if (checkName("from")) return fromImportStatement();
  if (checkName("type") && peek(1).type == TokenType::Name &&
      peek(2).type == TokenType::Op && peek(2).text == "=") {
    // Python 3.12 的 type 别名语句。识别出来并拒绝，比报「语法错误」有用。
    return rejectRemainder(Kind::TypeAlias);
  }

  // ── 赋值 / 增量赋值 / 类型注解 / 表达式语句 ──
  NodePtr first = expressionList();
  if (!first) return nullptr;

  const Token& after = peek();
  if (after.type == TokenType::Op && isAugAssignOp(after.text)) {
    const Kind op = augToBinary(after.text);
    const int opLine = after.line;
    advance();
    NodePtr value = expressionList();
    if (!value) return nullptr;
    auto node = make_node(Kind::AugAssign, token.line, token.col);
    node->ops.push_back(op);
    node->opLines.push_back(opLine);
    node->children.push_back(std::move(first));
    node->children.push_back(std::move(value));
    return node;
  }

  if (checkOp("=")) {
    auto node = make_node(Kind::Assign, token.line, token.col);
    node->children.push_back(std::move(first));
    while (matchOp("=")) {
      NodePtr value = expressionList();
      if (!value) return nullptr;
      node->children.push_back(std::move(value));
    }
    return node;
  }

  if (checkOp(":")) {
    advance();
    NodePtr annotation = expression();
    if (!annotation) return nullptr;
    auto node = make_node(Kind::AnnAssign, token.line, token.col);
    node->children.push_back(std::move(first));
    node->children.push_back(std::move(annotation));
    if (matchOp("=")) {
      NodePtr value = expressionList();
      if (!value) return nullptr;
      node->children.push_back(std::move(value));
    }
    return node;
  }

  auto node = make_node(Kind::Expr, token.line, token.col);
  node->children.push_back(std::move(first));
  return node;
}

NodePtr Parser::compoundStatement() {
  if (checkName("if")) return ifStatement();
  if (checkName("while")) return whileStatement();
  if (checkName("for")) return forStatement();
  if (checkName("def")) return functionDef(false);
  if (checkName("class")) return classDef();
  if (checkName("with")) return withStatement(false);

  if (checkName("async")) {
    const Token& token = peek();
    advance();
    if (checkName("def")) return functionDef(true);
    if (checkName("for")) {
      // 报错位置在语句本身。`async for` 要异步迭代器，本语言没有事件循环；
      // 留在这里明确拒绝，比让它退化成普通 for 安全。
      fail("'async for' is not supported");
      return nullptr;
    }
    if (checkName("with")) {
      fail("'async with' is not supported");
      return nullptr;
    }
    fail(diag::nameExpected("async"));
    return nullptr;
  }
  if (checkName("try")) return tryStatement();
  if (checkName("match")) return matchStatement();

  fail(diag::unsupportedStatement(peek().text));
  return nullptr;
}

NodePtr Parser::tryStatement() {
  auto node = make_node(Kind::Try, peek().line, peek().col);
  advance();

  // ── try 体 ──
  if (!parseBlock(&node->children)) return nullptr;
  node->bodyCount = node->children.size();

  // ── except 子句 ──
  //
  // 每个子句是 ExceptHandler：children = [异常类型?, 体...]，
  // text = `as` 的名字（空串表示没写）。
  //
  // 异常类型可以省略（裸 `except:`），也可以写多个（`except (A, B):`）。
  // 多个类型在类型位置是一个 TupleLit，解析器照常产出元组节点，
  // 由 codegen 展开成逐个匹配。
  for (;;) {
    skipNewlines();
    if (!checkName("except")) break;
    const Token& keyword = peek();
    advance();

    auto handler = make_node(Kind::ExceptHandler, keyword.line, keyword.col);

    // 没写类型就直接到冒号了（裸 `except:`）。
    const bool hasType = !checkOp(":");
    if (hasType) {
      NodePtr type = expression();
      if (!type) return nullptr;
      handler->children.push_back(std::move(type));
    }
    if (checkName("as")) {
      advance();
      if (!check(TokenType::Name)) {
        fail(diag::nameExpected("as"));
        return nullptr;
      }
      handler->text = peek().text;
      handler->hasExceptVar = true;
      advance();
    }
    const std::size_t beforeBody = handler->children.size();
    if (!parseBlock(&handler->children)) return nullptr;
    handler->handlerBodyCount = handler->children.size() - beforeBody;
    handler->hasType = hasType;
    node->children.push_back(std::move(handler));
    ++node->handlerCount;
  }

  // ── else 子句 ──
  //
  // 与 Python 一致：else 在没有任何异常时执行，且不被打包在保护区内。
  std::size_t orelseStart = node->children.size();
  skipNewlines();
  if (checkName("else")) {
    advance();
    if (!parseBlock(&node->children)) return nullptr;
  }
  node->orelseCount = node->children.size() - orelseStart;

  // ── finally 子句 ──
  std::size_t finalStart = node->children.size();
  skipNewlines();
  if (checkName("finally")) {
    advance();
    if (!parseBlock(&node->children)) return nullptr;
  }
  node->finalCount = node->children.size() - finalStart;

  // 一个 try 至少要有一个 except 或 finally，否则语法错误（与 CPython 一致）。
  if (node->handlerCount == 0 && node->finalCount == 0) {
    fail(diag::tryNeedsHandler());
    return nullptr;
  }
  return node;
}

NodePtr Parser::raiseStatement() {
  auto node = make_node(Kind::Raise, peek().line, peek().col);
  advance();

  // `raise` 单独一行（bare raise）表示重抛当前异常，只在 except 体里有意义。
  // 与 CPython 一致：内容为空就是重抛，由 codegen 发 RERAISE。
  if (check(TokenType::Newline) || checkOp(";") || check(TokenType::End)) {
    return node;
  }

  NodePtr exception = expression();
  if (!exception) return nullptr;
  node->children.push_back(std::move(exception));

  // `raise Exc from cause`：CPython 3 的语法。我们只解析掉，不做链式
  // traceback——没有 traceback 可链，`from` 不产生任何可观察差异。
  if (checkName("from")) {
    advance();
    NodePtr cause = expression();
    if (!cause) return nullptr;
    // 不保留 cause：它只影响 traceback 的显示，而我们只输出 `类型: 消息`。
  }
  return node;
}

NodePtr Parser::matchStatement() {
  auto node = make_node(Kind::Match, peek().line, peek().col);
  advance();
  NodePtr subject = expression();
  if (!subject) return nullptr;
  node->children.push_back(std::move(subject));
  if (!expectOp(":")) return nullptr;
  if (check(TokenType::Newline)) advance();
  if (!check(TokenType::Indent)) return fail(diag::blockExpected()), nullptr;
  advance();
  skipNewlines();
  while (!check(TokenType::Dedent) && !check(TokenType::End) && !failed_) {
    // case ...: 整段跳过
    while (!checkOp(":") && !check(TokenType::End)) advance();
    std::vector<NodePtr> ignored;
    if (!parseBlock(&ignored)) return nullptr;
    skipNewlines();
  }
  if (check(TokenType::Dedent)) advance();
  return node;
}

bool Parser::parseBlock(std::vector<NodePtr>* body) {
  if (!expectOp(":")) return false;
  if (check(TokenType::Newline)) {
    advance();
    skipNewlines();
    if (!check(TokenType::Indent)) return fail(diag::blockExpected());
    advance();
    skipNewlines();
    while (!check(TokenType::Dedent) && !check(TokenType::End) && !failed_) {
      NodePtr node = statement();
      if (!node) return false;
      if (node->kind == Kind::StatementList) {
        for (auto& child : node->children) body->push_back(std::move(child));
      } else {
        body->push_back(std::move(node));
      }
      skipNewlines();
    }
    if (check(TokenType::Dedent)) advance();
    return !failed_;
  }

  // 单行形式：`if x: y = 1`
  NodePtr node = smallStatement();
  if (!node) return false;
  if (node->kind == Kind::StatementList) {
    for (auto& child : node->children) body->push_back(std::move(child));
  } else {
    body->push_back(std::move(node));
  }
  return true;
}

NodePtr Parser::ifStatement() {
  // elif 在 AST 里是「orelse 里只有一条 If」，与 CPython 的形状一致。
  const Token& token = peek();
  advance();  // 'if'

  auto outer = make_node(Kind::If, token.line, token.col);
  NodePtr test = expression();
  if (!test) return nullptr;
  std::vector<NodePtr> body;
  if (!parseBlock(&body)) return nullptr;
  outer->children.push_back(std::move(test));
  for (auto& item : body) outer->children.push_back(std::move(item));
  outer->bodyCount = body.size();

  Node* current = outer.get();
  for (;;) {
    skipNewlines();
    if (checkName("elif")) {
      const Token& elifToken = peek();
      advance();
      auto nested = make_node(Kind::If, elifToken.line, elifToken.col);
      NodePtr elifTest = expression();
      if (!elifTest) return nullptr;
      std::vector<NodePtr> elifBody;
      if (!parseBlock(&elifBody)) return nullptr;
      nested->children.push_back(std::move(elifTest));
      for (auto& item : elifBody) nested->children.push_back(std::move(item));
      nested->bodyCount = elifBody.size();

      Node* raw = nested.get();
      current->children.push_back(std::move(nested));
      current->orelseCount = 1;
      current = raw;
      continue;
    }
    if (checkName("else")) {
      advance();
      std::vector<NodePtr> elseBody;
      if (!parseBlock(&elseBody)) return nullptr;
      for (auto& item : elseBody) current->children.push_back(std::move(item));
      current->orelseCount = elseBody.size();
    }
    break;
  }
  return outer;
}

NodePtr Parser::whileStatement() {
  const Token& token = peek();
  advance();
  auto node = make_node(Kind::While, token.line, token.col);
  NodePtr test = expression();
  if (!test) return nullptr;
  std::vector<NodePtr> body;
  if (!parseBlock(&body)) return nullptr;
  node->children.push_back(std::move(test));
  for (auto& item : body) node->children.push_back(std::move(item));
  node->bodyCount = body.size();

  skipNewlines();
  if (checkName("else")) {
    advance();
    std::vector<NodePtr> orelse;
    if (!parseBlock(&orelse)) return nullptr;
    for (auto& item : orelse) node->children.push_back(std::move(item));
    node->orelseCount = orelse.size();
  }
  return node;
}

NodePtr Parser::forStatement() {
  const Token& token = peek();
  advance();
  auto node = make_node(Kind::For, token.line, token.col);

  // 目标用 targetList()：它不把 `in` 当比较运算符，否则
  // `for i in [1]` 会把「循环变量」解析成一个布尔表达式。
  NodePtr target = targetList();
  if (!target) return nullptr;
  if (!matchName("in")) {
    fail(diag::missingInKeyword());
    return nullptr;
  }  NodePtr iter = expressionList();
  if (!iter) return nullptr;
  std::vector<NodePtr> body;
  if (!parseBlock(&body)) return nullptr;

  node->children.push_back(std::move(target));
  node->children.push_back(std::move(iter));
  for (auto& item : body) node->children.push_back(std::move(item));
  node->bodyCount = body.size();

  skipNewlines();
  if (checkName("else")) {
    advance();
    std::vector<NodePtr> orelse;
    if (!parseBlock(&orelse)) return nullptr;
    for (auto& item : orelse) node->children.push_back(std::move(item));
    node->orelseCount = orelse.size();
  }
  return node;
}

NodePtr Parser::parseFunctionArgs() {
  auto args = make_node(Kind::Arguments, peek().line, peek().col);
  if (!expectOp("(")) return nullptr;

  bool seenStar = false;
  bool seenKwonly = false;

  while (!checkOp(")") && !check(TokenType::End)) {
    if (matchOp("/")) {
      args->hasPosonly = true;
      if (args->posonlyLine == 0) {
        args->posonlyLine = peek().line;
        args->posonlyCol = peek().col;
      }
      if (!matchOp(",")) break;
      continue;
    }
    // 单独的 `*` 分隔符：`def f(*, a)`
    if (checkOp("*") && peek(1).type == TokenType::Op && peek(1).text == ",") {
      advance();
      advance();
      args->hasKwonly = true;
      seenKwonly = true;
      continue;
    }
    if (matchOp("*")) {
      args->hasVararg = true;
      seenStar = true;
      if (check(TokenType::Name)) args->varargName = advance().text;
      if (!matchOp(",")) break;
      continue;
    }
    if (matchOp("**")) {
      args->hasKwarg = true;
      if (check(TokenType::Name)) args->kwargName = advance().text;
      if (!matchOp(",")) break;
      continue;
    }

    if (!check(TokenType::Name)) {
      fail(diag::nameExpected("a parameter list"));
      return nullptr;
    }
    const std::string name = advance().text;
    // 参数注解 `a: int`：忽略注解表达式。CPython 把它求值后塞进
    // __annotations__，而本语言没有反射面，所以只解析不生成代码。
    if (matchOp(":")) {
      NodePtr annotation = expression();
      if (!annotation) return nullptr;
    }
    const bool keywordOnly = seenStar || seenKwonly;
    if (keywordOnly) {
      args->hasKwonly = true;
      args->kwonlyParams.push_back(name);
    } else {
      args->params.push_back(name);
    }

    // 默认值表达式跟着它自己的参数走，而不是按书写顺序堆在一起：
    // 仅关键字参数的默认值可以不在尾部（`def f(*, a=1, b)` 合法），
    // 顺序拼接就还原不出「哪个参数有默认值」。
    NodePtr value;
    if (matchOp("=")) {
      value = expression();
      if (!value) return nullptr;
    }
    if (keywordOnly) {
      args->kwonlyDefaults.push_back(std::move(value));
    } else {
      args->paramDefaults.push_back(std::move(value));
    }
    if (!matchOp(",")) break;
  }

  if (!expectOp(")")) return nullptr;
  return args;
}

NodePtr Parser::lambdaArgs() {
  auto args = make_node(Kind::Arguments, peek().line, peek().col);
  bool seenStar = false;
  bool seenKwonly = false;

  while (!checkOp(":") && !check(TokenType::End)) {
    if (matchOp("*")) {
      args->hasVararg = true;
      seenStar = true;
      if (check(TokenType::Name)) args->varargName = advance().text;
      if (!matchOp(",")) break;
      continue;
    }
    if (matchOp("**")) {
      args->hasKwarg = true;
      if (check(TokenType::Name)) args->kwargName = advance().text;
      if (!matchOp(",")) break;
      continue;
    }
    if (!check(TokenType::Name)) {
      fail(diag::nameExpected("a lambda parameter list"));
      return nullptr;
    }
    const std::string name = advance().text;
    const bool keywordOnly = seenStar || seenKwonly;
    if (keywordOnly) {
      args->hasKwonly = true;
      args->kwonlyParams.push_back(name);
    } else {
      args->params.push_back(name);
    }
    NodePtr value;
    if (matchOp("=")) {
      value = expression();
      if (!value) return nullptr;
    }
    if (keywordOnly) {
      args->kwonlyDefaults.push_back(std::move(value));
    } else {
      args->paramDefaults.push_back(std::move(value));
    }
    if (!matchOp(",")) break;
  }
  return args;
}

NodePtr Parser::functionDef(bool async) {
  const Token& token = peek();
  advance();  // 'def'
  auto node = make_node(async ? Kind::AsyncFunctionDef : Kind::FunctionDef,
                        token.line, token.col);
  if (!check(TokenType::Name)) {
    fail(diag::nameExpected("def"));
    return nullptr;
  }
  node->text = advance().text;

  NodePtr args = parseFunctionArgs();
  if (!args) return nullptr;

  // 返回注解 `-> int`：忽略（LANGUAGE.md 明确说被忽略）
  if (matchOp("->")) {
    NodePtr annotation = expression();
    if (!annotation) return nullptr;
  }

  std::vector<NodePtr> body;
  if (!parseBlock(&body)) return nullptr;
  node->children.push_back(std::move(args));
  for (auto& item : body) node->children.push_back(std::move(item));
  node->bodyCount = body.size();
  return node;
}

NodePtr Parser::classDef() {
  const Token& token = peek();
  advance();
  auto node = make_node(Kind::ClassDef, token.line, token.col);
  if (!check(TokenType::Name)) {
    fail(diag::nameExpected("class"));
    return nullptr;
  }
  node->text = advance().text;

  if (matchOp("(")) {
    while (!checkOp(")") && !check(TokenType::End)) {
      NodePtr base = expression();
      if (!base) return nullptr;
      node->children.push_back(std::move(base));
      if (!matchOp(",")) break;
    }
    if (!expectOp(")")) return nullptr;
  }
  node->argsCount = node->children.size();  // 基类个数

  std::vector<NodePtr> body;
  if (!parseBlock(&body)) return nullptr;
  for (auto& item : body) node->children.push_back(std::move(item));
  node->bodyCount = body.size();
  return node;
}

NodePtr Parser::withStatement(bool async) {
  const Token& token = peek();
  advance();
  auto node = make_node(async ? Kind::AsyncWith : Kind::With, token.line, token.col);
  for (;;) {
    auto item = make_node(Kind::WithItem, peek().line, peek().col);
    NodePtr context = expression();
    if (!context) return nullptr;
    item->children.push_back(std::move(context));
    if (matchName("as")) {
      // `as` 后面在语法上只能是名字或属性；按表达式解析会把它接成
      // `a as b, c` 这样的元组，反而给不出准确文案。
      NodePtr target = atomExpr();
      if (!target) return nullptr;
      item->children.push_back(std::move(target));
    }
    node->children.push_back(std::move(item));
    if (!matchOp(",")) break;
  }
  node->argsCount = node->children.size();  // with 项个数

  std::vector<NodePtr> body;
  if (!parseBlock(&body)) return nullptr;
  for (auto& item : body) node->children.push_back(std::move(item));
  node->bodyCount = body.size();
  return node;
}

}  // namespace pcc
