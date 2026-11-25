// 词法记号的定义。
#pragma once

#include <string>

namespace pcc {

enum class TokenType {
  Name,     // 标识符与关键字（关键字由语法层按文本判断）
  Number,   // 数字字面量
  String,   // 字符串字面量（已解码）
  Op,       // 运算符与分隔符
  Newline,  // 逻辑行结束
  Indent,
  Dedent,
  End,
};

struct Token {
  TokenType type = TokenType::End;
  std::string text;  // Name/Op 的文本；String 的解码后内容
  std::string raw;   // 原始文本（数字用）
  int line = 1;
  int col = 0;       // 0-based 字节列，与 Python ast 一致

  bool isFloat = false;
  bool isBytes = false;
  bool isFString = false;
  bool isEllipsis = false;
  bool isComplex = false;
};

}  // namespace pcc
