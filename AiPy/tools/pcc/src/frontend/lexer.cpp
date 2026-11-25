// 词法分析的实现。
//
// ── 缩进：为什么用栈 ──
//
// Python 的块结构由缩进决定，而解析器要看到的是扁平的 INDENT/DEDENT 记号。
// 每行行首把当前缩进宽度与栈顶比：更大就压栈并吐 INDENT，更小就连续弹栈、
// 每弹一层吐一个 DEDENT，相等就什么都不吐。与 CPython 的 tokenizer 一致。
//
// ── 括号内的换行不算断行 ──
//
// `f(a,\n  b)` 是合法的。跟踪括号深度，深度 >0 时把换行当空白吞掉。
// 这是「隐式续行」；反斜杠续行是另一套（显式），两者都要有。
#include "pcc/frontend/lexer.h"

#include "pcc/diagnostics.h"

#include <cctype>
#include <cstdlib>

namespace pcc {
namespace {

bool isNameStart(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' ||
         // 非 ASCII 字节一律当标识符字符：中文变量名在 Python 里合法，
         // 按 UTF-8 字节判断即可，不需要真正解码。
         static_cast<unsigned char>(ch) >= 0x80;
}

bool isNameChar(char ch) {
  return isNameStart(ch) || (ch >= '0' && ch <= '9');
}

bool isDigit(char ch) { return ch >= '0' && ch <= '9'; }

int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

void appendUtf8(std::uint32_t code, std::string* out) {
  if (code <= 0x7F) {
    out->push_back(static_cast<char>(code));
  } else if (code <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (code >> 6)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (code >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

// 三字符与两字符运算符。长串必须优先匹配，否则 `**` 会被切成两个 `*`。
const char* const kThreeChar[] = {"**=", "//=", ">>=", "<<=", "...", nullptr};
const char* const kTwoChar[] = {
    "**", "//", ">>", "<<", "<=", ">=", "==", "!=", "+=", "-=", "*=", "/=",
    "%=", "&=", "|=", "^=", "->", ":=", nullptr,
};

}  // namespace

Lexer::Lexer(std::string source, std::string filename)
    : source_(std::move(source)), filename_(std::move(filename)) {}

char Lexer::peek(std::size_t ahead) const {
  const std::size_t at = pos_ + ahead;
  return at < source_.size() ? source_[at] : '\0';
}

void Lexer::advance() {
  if (pos_ >= source_.size()) return;
  if (source_[pos_] == '\n') {
    ++line_;
    col_ = 0;
  } else {
    ++col_;
  }
  ++pos_;
}

void Lexer::emit(TokenType type, std::string text, int line, int col) {
  Token token;
  token.type = type;
  token.text = std::move(text);
  token.line = line;
  token.col = col;
  tokens_.push_back(std::move(token));
}

void Lexer::fail(const std::string& message) {
  if (failed_) return;
  failed_ = true;
  error_.message = message;
  error_.line = line_;
  error_.col = col_;
}

void Lexer::skipSpacesAndComment() {
  for (;;) {
    const char ch = peek();
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v') {
      advance();
      continue;
    }
    if (ch == '#') {
      while (!atEnd() && peek() != '\n') advance();
      continue;
    }
    // 显式续行：`\` 后面紧跟换行，整个换行被吞掉。
    if (ch == '\\' && (peek(1) == '\n' || (peek(1) == '\r' && peek(2) == '\n'))) {
      advance();
      if (peek() == '\r') advance();
      if (peek() == '\n') advance();
      continue;
    }
    break;
  }
}

bool Lexer::handleLineStart() {
  int width = 0;
  const int line = line_;
  while (!atEnd()) {
    const char ch = peek();
    if (ch == ' ') {
      ++width;
      advance();
    } else if (ch == '\t') {
      // tab 按 8 对齐，与 CPython 一致。
      width = (width / 8 + 1) * 8;
      advance();
    } else {
      break;
    }
  }

  const char next = peek();
  // 空行与纯注释行不产生缩进变化，也不产生 NEWLINE。
  if (next == '\n' || next == '\r' || next == '#' || next == '\0') {
    return false;
  }

  if (width > indents_.back()) {
    indents_.push_back(width);
    emit(TokenType::Indent, std::string(), line, 0);
  } else if (width < indents_.back()) {
    while (indents_.size() > 1 && width < indents_.back()) {
      indents_.pop_back();
      emit(TokenType::Dedent, std::string(), line, 0);
    }
    if (width != indents_.back()) {
      fail(diag::indentationMismatch());
      return false;
    }
  }
  return true;
}

bool Lexer::scanName() {
  const int line = line_;
  const int col = col_;
  const std::size_t begin = pos_;
  while (!atEnd() && isNameChar(peek())) advance();
  const std::string text = source_.substr(begin, pos_ - begin);

  // 字符串前缀：r / b / f / u 及其组合（rb, fr, ...）。
  const char ch = peek();
  if ((ch == '"' || ch == '\'') && !text.empty() && text.size() <= 2) {
    bool raw = false, bytes = false, fstring = false, valid = true;
    for (char letter : text) {
      switch (letter) {
        case 'r': case 'R': raw = true; break;
        case 'b': case 'B': bytes = true; break;
        case 'f': case 'F': fstring = true; break;
        case 'u': case 'U': break;  // u"" 就是普通字符串
        default: valid = false; break;
      }
    }
    if (valid && !(raw && bytes && fstring)) {
      Token token;
      token.type = TokenType::String;
      token.line = line;
      token.col = col;
      token.isBytes = bytes;
      token.isFString = fstring;
      return scanStringFromBody(&token, raw);
    }
    // 前缀之后不是字符串：那只是个名字，比如 `f` 后面接 `(`。
    emit(TokenType::Name, text, line, col);
    return true;
  }

  emit(TokenType::Name, text, line, col);
  return true;
}

bool Lexer::scanStringFromBody(Token* token, bool raw) {
  const char quote = peek();
  if (quote != '"' && quote != '\'') {
    fail(diag::missingQuote());
    return false;
  }

  bool triple = false;
  if (peek(1) == quote && peek(2) == quote) {
    triple = true;
    advance();
    advance();
    advance();
  } else {
    advance();
  }

  std::string value;
  for (;;) {
    if (atEnd()) {
      fail(diag::unterminatedString());
      return false;
    }
    const char ch = peek();

    if (triple) {
      if (ch == quote && peek(1) == quote && peek(2) == quote) {
        advance();
        advance();
        advance();
        break;
      }
    } else {
      if (ch == quote) {
        advance();
        break;
      }
      if (ch == '\n') {
        fail(diag::newlineInString());
        return false;
      }
    }

    if (ch == '\\' && !raw) {
      advance();
      if (atEnd()) {
        fail(diag::unterminatedString());
        return false;
      }
      const char escape = peek();
      advance();
      switch (escape) {
        case '\n': break;  // 续行：直接吞掉
        case '\\': value.push_back('\\'); break;
        case '\'': value.push_back('\''); break;
        case '"': value.push_back('"'); break;
        case 'a': value.push_back('\a'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'v': value.push_back('\v'); break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
          int code = escape - '0';
          for (int i = 0; i < 2 && peek() >= '0' && peek() <= '7'; ++i) {
            code = code * 8 + (peek() - '0');
            advance();
          }
          value.push_back(static_cast<char>(code & 0xFF));
          break;
        }
        case 'x': {
          int code = 0;
          for (int i = 0; i < 2; ++i) {
            const int digit = hexValue(peek());
            if (digit < 0) {
              fail(diag::badHexEscape(2));
              return false;
            }
            code = code * 16 + digit;
            advance();
          }
          value.push_back(static_cast<char>(code));
          break;
        }
        case 'u':
        case 'U': {
          const int digits = (escape == 'u') ? 4 : 8;
          std::uint32_t code = 0;
          for (int i = 0; i < digits; ++i) {
            const int digit = hexValue(peek());
            if (digit < 0) {
              fail(diag::badHexEscape(digits));
              return false;
            }
            code = code * 16 + static_cast<std::uint32_t>(digit);
            advance();
          }
          appendUtf8(code, &value);
          break;
        }
        default:
          // 不认识的转义：Python 保留反斜杠与字符本身。
          value.push_back('\\');
          value.push_back(escape);
          break;
      }
      continue;
    }

    value.push_back(ch);
    advance();
  }

  token->text = std::move(value);
  tokens_.push_back(std::move(*token));
  return true;
}

bool Lexer::scanNumber() {
  const int line = line_;
  const int col = col_;
  const std::size_t begin = pos_;

  bool isFloat = false;
  bool isComplex = false;

  if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' ||
                        peek(1) == 'B' || peek(1) == 'o' || peek(1) == 'O')) {
    advance();
    advance();
    while (!atEnd() && (isNameChar(peek()) || peek() == '_')) advance();
  } else {
    while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();
    if (peek() == '.' && (isDigit(peek(1)) || peek(1) == 'e' || peek(1) == 'E')) {
      isFloat = true;
      advance();
      while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();
    } else if (peek() == '.' && !isNameStart(peek(1))) {
      // `1.` 合法；`1.foo` 不合法，所以只在后面不是标识符时吃点。
      isFloat = true;
      advance();
    }
    if (peek() == 'e' || peek() == 'E') {
      isFloat = true;
      advance();
      if (peek() == '+' || peek() == '-') advance();
      while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();
    }
    if (peek() == 'j' || peek() == 'J') {
      isComplex = true;
      advance();
    }
  }

  Token token;
  token.type = TokenType::Number;
  token.raw = source_.substr(begin, pos_ - begin);
  token.text = token.raw;
  token.line = line;
  token.col = col;
  token.isFloat = isFloat;
  token.isComplex = isComplex;
  tokens_.push_back(std::move(token));
  return true;
}

bool Lexer::scanOperator() {
  const int line = line_;
  const int col = col_;

  for (const char* candidate : kThreeChar) {
    if (candidate == nullptr) break;
    if (source_.compare(pos_, 3, candidate) == 0) {
      advance();
      advance();
      advance();
      Token token;
      token.type = TokenType::Op;
      token.text = candidate;
      token.line = line;
      token.col = col;
      if (token.text == "...") token.isEllipsis = true;
      tokens_.push_back(std::move(token));
      return true;
    }
  }
  for (const char* candidate : kTwoChar) {
    if (candidate == nullptr) break;
    if (source_.compare(pos_, 2, candidate) == 0) {
      advance();
      advance();
      emit(TokenType::Op, candidate, line, col);
      return true;
    }
  }

  const char ch = peek();
  switch (ch) {
    case '(': case '[': case '{': ++depth_; break;
    case ')': case ']': case '}': if (depth_ > 0) --depth_; break;
    default: break;
  }
  advance();
  emit(TokenType::Op, std::string(1, ch), line, col);
  return true;
}

bool Lexer::tokenize() {
  bool atLineStart = true;

  while (!atEnd() && !failed_) {
    if (atLineStart && depth_ == 0) {
      if (!handleLineStart()) {
        if (failed_) break;
        // 空行 / 注释行：吃掉整行（含换行），继续。
        while (!atEnd() && peek() != '\n') advance();
        if (!atEnd()) advance();
        continue;
      }
      atLineStart = false;
      if (atEnd()) break;
    }

    skipSpacesAndComment();
    if (atEnd()) break;

    const char ch = peek();

    if (ch == '\n') {
      advance();
      if (depth_ == 0) {
        if (!tokens_.empty() && tokens_.back().type != TokenType::Newline &&
            tokens_.back().type != TokenType::Indent &&
            tokens_.back().type != TokenType::Dedent) {
          emit(TokenType::Newline, "\n", line_, col_);
          atLineStart = true;
        }
      }
      continue;
    }
    if (ch == '\r') {
      advance();
      continue;
    }

    if (isNameStart(ch)) {
      if (!scanName()) break;
      continue;
    }
    if (isDigit(ch) || (ch == '.' && isDigit(peek(1)))) {
      if (!scanNumber()) break;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      Token token;
      token.type = TokenType::String;
      token.line = line_;
      token.col = col_;
      if (!scanStringFromBody(&token, false)) break;
      continue;
    }
    if (!scanOperator()) break;
  }

  if (failed_) return false;

  // 收尾：补 NEWLINE 与全部 DEDENT。
  if (!tokens_.empty() && tokens_.back().type != TokenType::Newline) {
    Token token;
    token.type = TokenType::Newline;
    token.line = line_;
    token.col = col_;
    tokens_.push_back(std::move(token));
  }
  while (indents_.size() > 1) {
    indents_.pop_back();
    Token token;
    token.type = TokenType::Dedent;
    token.line = line_;
    token.col = 0;
    tokens_.push_back(std::move(token));
  }
  Token end;
  end.type = TokenType::End;
  end.line = line_;
  tokens_.push_back(std::move(end));
  return true;
}

}  // namespace pcc
