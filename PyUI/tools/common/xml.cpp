#include "xml.h"

#include <cstdio>
#include <cstdlib>

namespace pyui {
namespace xml {
namespace {

// 解析器状态。位置用字节偏移推进，行号列号随读入同步更新——
// 报错要指到「第几个字」，所以不能用「读完后数换行」的做法。
class Parser {
 public:
  Parser(std::string_view text) : text_(text) {}

  Document run() {
    Document doc;

    // 跳过 BOM 与 XML 声明
    skipBom();
    skipWhitespaceAndMisc();
    if (!ok_) {
      doc.error = error_;
      return doc;
    }

    if (pos_ >= text_.size() || text_[pos_] != '<') {
      fail("文件里没有根元素");
      doc.error = error_;
      return doc;
    }

    if (!parseElement(doc.root)) {
      doc.error = error_;
      return doc;
    }

    skipWhitespaceAndMisc();
    if (pos_ < text_.size()) {
      fail("根元素之后还有内容");
      doc.error = error_;
      return doc;
    }

    doc.ok = true;
    return doc;
  }

 private:
  void skipBom() {
    if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
        static_cast<unsigned char>(text_[1]) == 0xBB &&
        static_cast<unsigned char>(text_[2]) == 0xBF) {
      pos_ = 3;
    }
  }

  bool atEnd() const { return pos_ >= text_.size(); }
  char peek(size_t ahead = 0) const {
    const size_t at = pos_ + ahead;
    return at < text_.size() ? text_[at] : '\0';
  }

  void advance() {
    if (pos_ >= text_.size()) return;
    if (text_[pos_] == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    ++pos_;
  }

  void advanceTo(size_t target) {
    while (pos_ < target && pos_ < text_.size()) advance();
  }

  void fail(std::string message) {
    if (!ok_) return;  // 只保留第一个错误，后面的都是它的连锁反应
    ok_ = false;
    error_.message = std::move(message);
    error_.line = line_;
    error_.column = column_;
  }

  bool startsWith(std::string_view prefix) const {
    return text_.compare(pos_, prefix.size(), prefix) == 0;
  }

  void skipWhitespace() {
    while (!atEnd()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        advance();
      } else {
        break;
      }
    }
  }

  // 跳过空白、注释、处理指令、DOCTYPE
  void skipWhitespaceAndMisc() {
    while (!atEnd()) {
      skipWhitespace();
      if (startsWith("<!--")) {
        if (!skipComment()) return;
      } else if (startsWith("<?")) {
        if (!skipUntil("?>")) return;
      } else if (startsWith("<!")) {
        // DOCTYPE：跳过到匹配的 '>'，内部子集（[...]）也一并跳过
        if (!skipDoctype()) return;
      } else {
        return;
      }
    }
  }

  bool skipComment() {
    const size_t close = text_.find("-->", pos_);
    if (close == std::string_view::npos) {
      fail("注释没有闭合的 -->");
      return false;
    }
    advanceTo(close + 3);
    return true;
  }

  bool skipUntil(std::string_view terminator) {
    const size_t close = text_.find(terminator, pos_);
    if (close == std::string_view::npos) {
      fail("处理指令没有闭合");
      return false;
    }
    advanceTo(close + terminator.size());
    return true;
  }

  bool skipDoctype() {
    int depth = 0;
    while (!atEnd()) {
      const char c = text_[pos_];
      if (c == '[') ++depth;
      if (c == ']') --depth;
      if (c == '>' && depth <= 0) {
        advance();
        return true;
      }
      advance();
    }
    fail("DOCTYPE 没有闭合");
    return false;
  }

  // 标签名：字母、数字、下划线、点、连字符、冒号
  bool isNameChar(char c) const {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-' || c == ':';
  }

  std::string readName() {
    const size_t start = pos_;
    while (!atEnd() && isNameChar(text_[pos_])) advance();
    return std::string(text_.substr(start, pos_ - start));
  }

  bool parseElement(Node& node) {
    node.line = line_;
    node.column = column_;

    if (peek() != '<') {
      fail("期望 '<'");
      return false;
    }
    advance();

    node.tag = readName();
    if (node.tag.empty()) {
      fail("标签名为空");
      return false;
    }

    // 属性
    while (true) {
      skipWhitespace();
      if (atEnd()) {
        fail("<" + node.tag + "> 没有闭合");
        return false;
      }
      if (peek() == '/') {
        // 自闭合
        advance();
        if (peek() != '>') {
          fail("'/' 之后期望 '>'");
          return false;
        }
        advance();
        return true;
      }
      if (peek() == '>') {
        advance();
        break;
      }

      const int attrLine = line_;
      const int attrColumn = column_;
      std::string name = readName();
      if (name.empty()) {
        fail("属性名为空");
        return false;
      }
      skipWhitespace();
      if (peek() != '=') {
        fail("属性 " + name + " 缺少 '='");
        return false;
      }
      advance();
      skipWhitespace();

      const char quote = peek();
      if (quote != '"' && quote != '\'') {
        fail("属性 " + name + " 的值必须用引号包起来");
        return false;
      }
      advance();

      std::string value;
      if (!readAttributeValue(quote, value)) return false;

      for (const auto& kv : node.attributes) {
        if (kv.first == name) {
          fail("属性 " + name + " 重复出现");
          return false;
        }
      }
      (void)attrLine;
      (void)attrColumn;
      node.attributes.emplace_back(std::move(name), std::move(value));
    }

    // 子节点或文本
    while (true) {
      if (atEnd()) {
        fail("<" + node.tag + "> 没有闭合");
        return false;
      }

      if (startsWith("</")) {
        const int closeLine = line_;
        const int closeColumn = column_;
        advanceTo(pos_ + 2);
        const std::string closeTag = readName();
        skipWhitespace();
        if (peek() != '>') {
          fail("闭合标签缺少 '>'");
          return false;
        }
        advance();
        if (closeTag != node.tag) {
          // 报错要指向开标签，用户更容易看出是哪一层错配了
          ok_ = false;
          error_.message = "闭合标签 </" + closeTag + "> 与开标签 <" + node.tag + "> 不匹配";
          error_.line = closeLine;
          error_.column = closeColumn;
          (void)closeLine;
          (void)closeColumn;
          return false;
        }
        return true;
      }

      if (startsWith("<!--")) {
        if (!skipComment()) return false;
        continue;
      }

      if (startsWith("<![CDATA[")) {
        const size_t close = text_.find("]]>", pos_);
        if (close == std::string_view::npos) {
          fail("CDATA 没有闭合");
          return false;
        }
        const size_t start = pos_ + 9;
        node.text.append(text_.substr(start, close - start));
        advanceTo(close + 3);
        continue;
      }

      if (startsWith("<?")) {
        if (!skipUntil("?>")) return false;
        continue;
      }

      if (peek() == '<') {
        Node child;
        if (!parseElement(child)) return false;
        node.children.push_back(std::move(child));
        continue;
      }

      // 文本：积累到下一个 '<'。布局里通常只有空白，但保留下来便于诊断。
      const size_t start = pos_;
      while (!atEnd() && peek() != '<') advance();
      node.text.append(decodeEntities(text_.substr(start, pos_ - start)));
    }
  }

  bool readAttributeValue(char quote, std::string& out) {
    std::string raw;
    while (!atEnd()) {
      const char c = text_[pos_];
      if (c == quote) {
        advance();
        out = decodeEntities(raw);
        return true;
      }
      if (c == '<') {
        fail("属性值里不能出现 '<'");
        return false;
      }
      raw.push_back(c);
      advance();
    }
    fail("属性值没有闭合的引号");
    return false;
  }

  // 只处理五个内置实体与数字引用。布局里写 &nbsp; 这类 HTML 实体是错的，
  // HTML 实体不在 XML 规范里——不支持才能让错误暴露出来。
  static std::string decodeEntities(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
      if (raw[i] != '&') {
        out.push_back(raw[i]);
        ++i;
        continue;
      }
      const size_t semi = raw.find(';', i);
      if (semi == std::string_view::npos) {
        out.push_back(raw[i]);
        ++i;
        continue;
      }
      const std::string_view entity = raw.substr(i + 1, semi - i - 1);
      if (entity == "lt") {
        out.push_back('<');
      } else if (entity == "gt") {
        out.push_back('>');
      } else if (entity == "amp") {
        out.push_back('&');
      } else if (entity == "quot") {
        out.push_back('"');
      } else if (entity == "apos") {
        out.push_back('\'');
      } else if (!entity.empty() && entity[0] == '#') {
        const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
        const std::string digits(entity.substr(hex ? 2 : 1));
        const unsigned long cp = std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10);
        appendUtf8(out, static_cast<unsigned int>(cp));
      } else {
        // 未知实体原样保留，交给上层报错（比静默吞掉更容易发现）
        out.append(raw.substr(i, semi - i + 1));
      }
      i = semi + 1;
    }
    return out;
  }

  static void appendUtf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  std::string_view text_;
  size_t pos_ = 0;
  int line_ = 1;
  int column_ = 1;
  bool ok_ = true;
  Error error_;
};

}  // namespace

Document parse(std::string_view text) {
  Parser parser(text);
  return parser.run();
}

Document parseFile(const std::string& path) {
  Document doc;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    doc.error.message = "打不开文件：" + path;
    return doc;
  }
  std::string contents;
  char buffer[4096];
  while (true) {
    const size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    contents.append(buffer, read);
    if (read < sizeof(buffer)) break;
  }
  std::fclose(file);
  return parse(contents);
}

}  // namespace xml
}  // namespace pyui
