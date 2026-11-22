// pyui_layoutc：把布局 XML 编译成 C++ 工厂函数
//
// 用法：
//   pyui_layoutc --out <生成目录> <布局 XML>...
//
// 每个 XML 生成两个东西：
//   <stem>.gen.cpp          工厂函数，直接 new + setter，运行期零解析
//   pyui_layouts.gen.cpp     汇总注册表（把所有工厂登记进 LayoutInflater）
//
// 产物形态刻意不生成布局类：
//
//   namespace pyui::generated {
//   ViewGroup* createMainFrame() {
//     auto* root = new pyui::LinearLayout(pyui::Orientation::kVertical);
//     ...
//     return root;
//   }
//   }
//
//   // 汇总文件里：
//   const LayoutRegistration kReg0("main_frame", &createMainFrame);
//
// 为什么没有 MainFrame 类：见 AGENTS.md 铁律四。一旦生成具名类，业务代码就会
// #include 它并写 frame->crumb，那时「按名字取布局」就退化成编译期强耦合，
// 换实现要改遍业务代码。
//
// 汇总文件的存在是为了绕开静态库的链接丢弃：工厂若只在静态库里且没人引用，
// 链接器会把整个目标文件扔掉，注册项随之消失。汇总文件被应用直接引用，所以
// 它引用的工厂一定会被保留。
//
// 所有校验在这里完成并报 file:line:col —— C++ 编译器做不到这件事。
#include "pyui/specs.h"
#include "xml.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// 解析器与属性表都在 pyui 下；生成器本身在全局匿名命名空间里跑，
// 用别名省得每处都写 pyui::xml::。
namespace xml = pyui::xml;

namespace {

// ── 诊断 ──

struct Diagnostic {
  std::string file;
  int line = 0;
  int column = 0;
  std::string message;
};

struct Context {
  std::string file;
  std::vector<Diagnostic>* sink = nullptr;
  // 本文件里 id → 首次出现的行号，用于查重
  std::map<std::string, int> ids;
  int errors = 0;

  void error(int line, int column, std::string message) {
    sink->push_back(Diagnostic{file, line, column, std::move(message)});
    ++errors;
  }
};

// ── 字符串工具 ──

std::string trim(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (begin < end && isSpace(text[begin])) ++begin;
  while (end > begin && isSpace(text[end - 1])) --end;
  return std::string(text.substr(begin, end - begin));
}

// 生成 C++ 字符串字面量。源文件按 UTF-8 编译，中文直接进字面量即可；
// 只需转义引号、反斜杠与控制字符。
std::string cppString(std::string_view text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\x%02X", c);
          out += buffer;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out += '"';
  return out;
}

// 文件名 → 合法 C++ 标识符片段（main_frame → MainFrame）
std::string identFromStem(const std::string& stem) {
  std::string out;
  bool upper = true;
  for (char c : stem) {
    const bool alnum =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alnum) {
      upper = true;
      continue;
    }
    if (upper && c >= 'a' && c <= 'z') {
      out.push_back(static_cast<char>(c - 32));
      upper = false;
    } else {
      out.push_back(c);
      upper = false;
    }
  }
  if (out.empty()) out = "Layout";
  if (out[0] >= '0' && out[0] <= '9') out.insert(out.begin(), '_');
  return out;
}

// ── 取值解析 ──

bool parseColor(std::string_view text, unsigned int& out) {
  std::string digits;
  size_t i = (!text.empty() && text[0] == '#') ? 1 : 0;
  const std::string_view rest = text.substr(i);
  if (rest.size() != 6 && rest.size() != 8) return false;
  for (char c : rest) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
    digits.push_back(c);
  }
  if (digits.size() == 6) digits.insert(0, "FF");
  out = static_cast<unsigned int>(std::strtoul(digits.c_str(), nullptr, 16));
  return true;
}

// big → TextSize::kBig
std::string enumConstant(const std::string& enumType, const std::string& value) {
  std::string name = "k";
  bool upper = true;
  for (char c : value) {
    if (c == '-' || c == '_') {
      upper = true;
      continue;
    }
    if (upper && c >= 'a' && c <= 'z') {
      name.push_back(static_cast<char>(c - 32));
      upper = false;
    } else {
      name.push_back(c);
      upper = false;
    }
  }
  return enumType + "::" + name;
}

bool parseBoolValue(const std::string& value, bool& out) {
  if (value == "true" || value == "1") {
    out = true;
    return true;
  }
  if (value == "false" || value == "0") {
    out = false;
    return true;
  }
  return false;
}

bool isInteger(const std::string& value) {
  if (value.empty()) return false;
  // 十六进制也接受：id 要能写 0x7A02，与 AiUI / Win32 的常量写法一致。
  const bool hex = value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
  char* end = nullptr;
  std::strtol(value.c_str(), &end, hex ? 16 : 10);
  return end != value.c_str() && *end == '\0';
}

bool isNumber(const std::string& value) {
  if (value.empty()) return false;
  char* end = nullptr;
  std::strtod(value.c_str(), &end);
  return end != value.c_str() && *end == '\0';
}

// 有没有小数点或指数部分：决定浮点字面量要不要补 ".0"。
// C++ 里 `1f` 不是合法字面量，`1.0f` 才是；而 `1e3f` 也是合法的（带指数）。
bool hasDecimalPoint(const std::string& value) {
  return value.find('.') != std::string::npos || value.find('e') != std::string::npos ||
         value.find('E') != std::string::npos;
}

// 尺寸：match / wrap / 整数（96 DPI 设计值）
bool parseDimension(const std::string& value, std::string& out) {
  if (value == "match") {
    out = "pyui::LayoutParams::kMatchParent";
    return true;
  }
  if (value == "wrap") {
    out = "pyui::LayoutParams::kWrapContent";
    return true;
  }
  if (!isInteger(value)) return false;
  // 设计值原样保留：LayoutParams 里存的就是 96 DPI 设计值，
  // 换算发生在 View::px()，那时才知道目标窗口的 DPI。
  out = value;
  return true;
}

bool parseGravity(const std::string& name, std::string& out) {
  if (name == "start") {
    out = "pyui::Gravity::kStart";
  } else if (name == "center") {
    out = "pyui::Gravity::kCenter";
  } else if (name == "end") {
    out = "pyui::Gravity::kEnd";
  } else {
    return false;
  }
  return true;
}

// 把 XML 的枚举取值翻译成 C++ 实参。集中在一处，避免六个 switch 分支各写一遍。
bool literalFor(const pyui::specs::AttributeSpec& attr, const std::string& value,
                std::string& out, std::string& why) {
  using pyui::specs::ValueType;
  switch (attr.type) {
    case ValueType::kText:
    case ValueType::kString:
      out = cppString(value);
      return true;
    case ValueType::kInt:
      if (!isInteger(value)) {
        why = "必须是整数";
        return false;
      }
      out = value;
      return true;
    case ValueType::kDesign:
      // 96 DPI 设计值，控件内部自己用 px() 换算
      if (!isInteger(value)) {
        why = "必须是整数（96 DPI 设计值）";
        return false;
      }
      out = value;
      return true;
    case ValueType::kFloat:
      if (!isNumber(value)) {
        why = "必须是数字";
        return false;
      }
      // "1" 加 f 后缀会得到非法的 1f；必须补上小数点
      out = hasDecimalPoint(value) ? value + "f" : value + ".0f";
      return true;
    case ValueType::kDouble:
      if (!isNumber(value)) {
        why = "必须是数字";
        return false;
      }
      // 整数字面量在 double 语境下自动提升，不必补小数点
      out = value;
      return true;
    case ValueType::kBool: {
      bool parsed = false;
      if (!parseBoolValue(value, parsed)) {
        why = "必须是 true 或 false";
        return false;
      }
      out = parsed ? "true" : "false";
      return true;
    }
    case ValueType::kColor: {
      unsigned int parsed = 0;
      if (!parseColor(value, parsed)) {
        why = "必须是 #RRGGBB 或 #AARRGGBB";
        return false;
      }
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "0x%08X", parsed);
      out = "pyui::Color{" + std::string(buffer) + "}";
      return true;
    }
    case ValueType::kEnum:
      out = "pyui::" + enumConstant(attr.enumType, value);
      return true;
    case ValueType::kId:
      out = "pyui::makeId(" + cppString(value) + ")";
      return true;
    case ValueType::kDimension:
      if (!parseDimension(value, out)) {
        why = "必须是 match / wrap / 整数";
        return false;
      }
      return true;
  }
  why = "未知的属性类型";
  return false;
}

// ── 生成器 ──

class Generator {
 public:
  Generator(Context& ctx, const xml::Node& root) : ctx_(ctx), root_(root) {}

  // 生成整棵树的代码，rootVar 返回根变量名
  // depth 从 1 起：根节点也要缩进，与函数体的其它语句对齐。
  bool emit(std::string& body, std::string& rootVar) {
    if (!emitNode(root_, "", 1, body, rootVar)) return false;
    return true;
  }

 private:
  std::string indent(int depth) const { return std::string(static_cast<size_t>(depth) * 2, ' '); }

  bool validate(const xml::Node& node, const pyui::specs::WidgetSpec& spec);
  bool emitNode(const xml::Node& node, const std::string& parentVar, int depth, std::string& out,
                std::string& varOut);
  bool emitLayoutParams(const xml::Node& node, const pyui::specs::WidgetSpec& spec,
                        std::string& out);
  bool emitSetters(const xml::Node& node, const pyui::specs::WidgetSpec& spec,
                   const std::string& var, std::string& out);

  Context& ctx_;
  const xml::Node& root_;
  int counter_ = 0;
};

bool Generator::validate(const xml::Node& node, const pyui::specs::WidgetSpec& spec) {
  bool ok = true;
  std::vector<std::pair<std::string, std::string>> raw;

  for (const auto& kv : node.attributes) {
    raw.push_back(kv);
    const pyui::specs::AttributeSpec* attr = pyui::specs::findAttribute(spec.tag, kv.first);
    if (attr == nullptr) {
      std::string available;
      for (const auto& candidate : spec.attributes) {
        if (!available.empty()) available += " / ";
        available += candidate.name;
      }
      ctx_.error(node.line, node.column,
                 "<" + spec.tag + "> 没有属性 " + kv.first +
                     (available.empty() ? "" : "，可用：" + available));
      ok = false;
      continue;
    }

    // 枚举取值域单独报，错误信息能直接列出合法取值
    if (attr->type == pyui::specs::ValueType::kEnum) {
      bool matched = false;
      std::string allowedList;
      for (const auto& allowed : attr->allowedValues) {
        if (allowed == kv.second) matched = true;
        if (!allowedList.empty()) allowedList += " / ";
        allowedList += allowed;
      }
      if (!matched) {
        ctx_.error(node.line, node.column,
                   "<" + spec.tag + "> 的 " + kv.first + "=\"" + kv.second +
                       "\" 不合法，只能是：" + allowedList);
        ok = false;
      }
      continue;
    }

    std::string unused;
    std::string why;
    if (!literalFor(*attr, kv.second, unused, why)) {
      ctx_.error(node.line, node.column,
                 "<" + spec.tag + "> 的 " + kv.first + " " + why + "（当前值：" + kv.second + "）");
      ok = false;
    }
  }

  const std::string combination = pyui::specs::validateCombination(spec.tag, raw);
  if (!combination.empty()) {
    ctx_.error(node.line, node.column, "<" + spec.tag + "> " + combination);
    ok = false;
  }

  return ok;
}

bool Generator::emitLayoutParams(const xml::Node& node, const pyui::specs::WidgetSpec& spec,
                                 std::string& out) {
  // 逐项拼 LayoutParams，不生成「运行期按属性名分支」的代码——
  // 那样又回到字符串比较了。
  bool hasWidth = false;
  bool hasHeight = false;
  std::string width;
  std::string height;
  std::string weight;
  std::string marginAll;
  std::string marginLeft, marginTop, marginRight, marginBottom;
  std::string offsetX, offsetY;
  std::string align;
  std::string maxWidth, maxHeight;

  for (const auto& kv : node.attributes) {
    const pyui::specs::AttributeSpec* attr = pyui::specs::findAttribute(spec.tag, kv.first);
    if (attr == nullptr) continue;

    std::string literal;
    std::string why;
    if (!literalFor(*attr, kv.second, literal, why)) continue;  // validate 已经报过了

    if (kv.first == "width") {
      hasWidth = true;
      width = literal;
    } else if (kv.first == "height") {
      hasHeight = true;
      height = literal;
    } else if (kv.first == "weight") {
      weight = literal;
    } else if (kv.first == "margin") {
      marginAll = literal;
    } else if (kv.first == "marginLeft") {
      marginLeft = literal;
    } else if (kv.first == "marginTop") {
      marginTop = literal;
    } else if (kv.first == "marginRight") {
      marginRight = literal;
    } else if (kv.first == "marginBottom") {
      marginBottom = literal;
    } else if (kv.first == "x") {
      offsetX = literal;
    } else if (kv.first == "y") {
      offsetY = literal;
    } else if (kv.first == "maxWidth") {
      maxWidth = literal;
    } else if (kv.first == "maxHeight") {
      maxHeight = literal;
    } else if (kv.first == "align") {
      align = kv.second;  // 需要拆成两个 Gravity，单独处理
    }
  }

  const bool hasParams = hasWidth || hasHeight || !weight.empty() || !marginAll.empty() ||
                         !marginLeft.empty() || !marginTop.empty() || !marginRight.empty() ||
                         !marginBottom.empty() || !offsetX.empty() || !offsetY.empty() ||
                         !align.empty() || !maxWidth.empty() || !maxHeight.empty();

  out += "pyui::LayoutParams{}";
  if (!hasParams) return true;

  if (hasWidth || hasHeight) {
    const std::string w = hasWidth ? width : "pyui::LayoutParams::kMatchParent";
    const std::string h = hasHeight ? height : "pyui::LayoutParams::kMatchParent";
    out += ".withSize(" + w + ", " + h + ")";
  }
  if (!weight.empty()) out += ".withWeight(" + weight + ")";
  if (!marginAll.empty()) {
    out += ".margin(" + marginAll + ")";
  } else if (!marginLeft.empty() || !marginTop.empty() || !marginRight.empty() ||
             !marginBottom.empty()) {
    out += ".margin(" + (marginLeft.empty() ? "0" : marginLeft) + ", " +
           (marginTop.empty() ? "0" : marginTop) + ", " +
           (marginRight.empty() ? "0" : marginRight) + ", " +
           (marginBottom.empty() ? "0" : marginBottom) + ")";
  }
  if (!offsetX.empty() || !offsetY.empty()) {
    out += ".offset(" + (offsetX.empty() ? "0" : offsetX) + ", " +
           (offsetY.empty() ? "0" : offsetY) + ")";
  }
  if (!align.empty()) {
    const size_t comma = align.find(',');
    const std::string first = trim(align.substr(0, comma));
    const std::string second =
        comma == std::string::npos ? std::string("start") : trim(align.substr(comma + 1));
    std::string horizontal;
    std::string vertical;
    if (!parseGravity(first, horizontal) || !parseGravity(second, vertical)) {
      // validate 阶段不查 align 的取值（它是 kString），在这里补报
      ctx_.error(node.line, node.column, "align 的取值只能是 start / center / end（写 \"水平,垂直\"）");
      return false;
    }
    out += ".align(" + horizontal + ", " + vertical + ")";
  }
  if (!maxWidth.empty() || !maxHeight.empty()) {
    out += ".withMax(" + (maxWidth.empty() ? "0" : maxWidth) + ", " +
           (maxHeight.empty() ? "0" : maxHeight) + ")";
  }
  return true;
}

bool Generator::emitSetters(const xml::Node& node, const pyui::specs::WidgetSpec& spec,
                            const std::string& var, std::string& out) {
  // paddingHorizontal/Vertical 只有一个时无法表达，成对出现才走两参重载
  const std::string* paddingH = node.attribute("paddingHorizontal");
  const std::string* paddingV = node.attribute("paddingVertical");
  const bool hasPaddingAll = node.attribute("padding") != nullptr;

  bool ok = true;

  for (const auto& kv : node.attributes) {
    const pyui::specs::AttributeSpec* attr = pyui::specs::findAttribute(spec.tag, kv.first);
    if (attr == nullptr || attr->layoutParam) continue;

    if (attr->type == pyui::specs::ValueType::kId) {
      // id 有两种写法：
      //   名字（"ok"）       → 编译期折成 makeId("ok")
      //   数值（"0x7A02"）  → 原样保留
      //
      // 为什么保留数值：内置控件的 id 是**跨库契约**。AiUI 的标题栏用
      // 0x7A00–0x7A08，同一份 XML 在两边都该指到同一个部件；如果强制折成
      // 哈希，同样的界面在两个库里就要写两套 id。
      auto inserted = ctx_.ids.emplace(kv.second, node.line);
      if (!inserted.second && inserted.first->second != node.line) {
        ctx_.error(node.line, node.column, "id=\"" + kv.second + "\" 与第 " +
                                               std::to_string(inserted.first->second) +
                                               " 行重复");
        ok = false;
        continue;
      }
      const bool numeric =
          isInteger(kv.second) && (std::isdigit(static_cast<unsigned char>(kv.second[0])) ||
                                   (kv.second[0] == '-' &&
                                    kv.second.size() > 1 &&
                                    std::isdigit(static_cast<unsigned char>(kv.second[1]))));
      if (numeric) {
        out += var + "->setId(" + kv.second + ");\n";
      } else {
        out += var + "->setId(pyui::makeId(" + cppString(kv.second) + "));\n";
      }
      continue;
    }

    // padding 系列要合并成一个调用，跳过单属性循环
    if (kv.first == "padding" || kv.first == "paddingHorizontal" || kv.first == "paddingVertical") {
      continue;
    }

    if (attr->setter.empty()) {
      ctx_.error(node.line, node.column, "spec 缺少属性 " + kv.first + " 的 setter");
      ok = false;
      continue;
    }

    // 需要额外参数、无法用单个字面量表达的属性
    if (spec.tag == "TextField" && kv.first == "password") {
      bool enabled = false;
      parseBoolValue(kv.second, enabled);
      out += var + "->setPasswordChar(0x25CF, " + std::string(enabled ? "true" : "false") + ");\n";
      continue;
    }

    std::string literal;
    std::string why;
    if (!literalFor(*attr, kv.second, literal, why)) continue;  // validate 已报

    out += var + "->" + attr->setter + "(" + literal + ");\n";
  }

  // padding：一个属性走单参，成对出现走两参，否则明确报错
  if (hasPaddingAll) {
    out += var + "->setPadding(" + *node.attribute("padding") + ");\n";
  } else if (paddingH != nullptr || paddingV != nullptr) {
    if (paddingH == nullptr || paddingV == nullptr) {
      ctx_.error(node.line, node.column,
                 "paddingHorizontal 与 paddingVertical 必须成对出现，或改用 padding");
      return false;
    }
    out += var + "->setPadding(" + *paddingH + ", " + *paddingV + ");\n";
  }

  return ok;
}

bool Generator::emitNode(const xml::Node& node, const std::string& parentVar, int depth,
                         std::string& out, std::string& varOut) {
  const pyui::specs::WidgetSpec* spec = pyui::specs::find(node.tag);

  // 收集诊断时不能遇到第一个错误就退出：一份 XML 里常有五六处笔误，
  // 一次只报一个会让人反复「改一处、重跑、再报一处」。下面每条出错路径
  // 都先把错误记下来，再继续遍历子节点，最后返回 false 表示这一支没生成。
  auto collectChildren = [&]() {
    for (const auto& child : node.children) {
      std::string ignored;
      emitNode(child, "", depth + 1, out, ignored);
    }
  };

  if (spec == nullptr) {
    // 猜一个最接近的标签名，报错时直接给出正确写法
    std::string suggestion;
    for (const auto& candidate : pyui::specs::all()) {
      if (candidate.tag.size() != node.tag.size()) continue;
      size_t diff = 0;
      for (size_t i = 0; i < candidate.tag.size(); ++i) {
        if (candidate.tag[i] != node.tag[i]) ++diff;
      }
      if (diff <= 2) {
        suggestion = candidate.tag;
        break;
      }
    }
    ctx_.error(node.line, node.column,
               "未知控件 <" + node.tag + ">" +
                   (suggestion.empty() ? "" : "，你是不是要 <" + suggestion + ">"));
    collectChildren();
    return false;
  }

  bool ok = validate(node, *spec);

  // 非空文本节点在布局里一定是写错了（文字要用 text 属性）
  const std::string text = trim(node.text);
  if (!text.empty()) {
    ctx_.error(node.line, node.column,
               "<" + node.tag + "> 里不能直接写文字，请用 text=\"" + text + "\"");
    ok = false;
  }

  if (!node.children.empty() && !spec->container) {
    ctx_.error(node.children.front().line, node.children.front().column,
               "<" + node.tag + "> 不是容器，不能包含子控件");
    ok = false;
  }

  // 出错时把出错的子节点也过一遍，但不再为它们生成代码
  if (!ok) {
    for (const auto& child : node.children) {
      std::string ignored;
      emitNode(child, "", depth + 1, out, ignored);
    }
    return false;
  }

  const std::string var = "_v" + std::to_string(++counter_);
  const std::string pad = indent(depth);
  varOut = var;

  out += pad + "auto* " + var + " = new pyui::" + spec->tag + "();\n";

  std::string setters;
  if (!emitSetters(node, *spec, var, setters)) return false;
  for (size_t start = 0; start < setters.size();) {
    const size_t end = setters.find('\n', start);
    const std::string line = setters.substr(start, end - start);
    if (!line.empty()) out += pad + line + "\n";
    if (end == std::string::npos) break;
    start = end + 1;
  }

  if (!parentVar.empty()) {
    std::string params;
    if (!emitLayoutParams(node, *spec, params)) return false;
    out += pad + parentVar + "->addView(" + var + ", " + params + ");\n";
  }

  bool childrenOk = true;
  for (const auto& child : node.children) {
    std::string childVar;
    if (!emitNode(child, var, depth + 1, out, childVar)) childrenOk = false;
  }

  return childrenOk;
}

// ── 文件 IO ──

// 读已有文件（用于「内容没变就不动 mtime」）。文件不存在返回 false。
bool readFile(const std::string& path, std::string& contents) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return false;
  contents.clear();
  char buffer[4096];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) contents.append(buffer, got);
  std::fclose(file);
  return true;
}

// 写文件；内容与磁盘上一模一样时**不写**。
//
// 这一条不是优化而是正确性：生成器一次把所有产物都写一遍，若每次都刷新
// mtime，构建系统会认为所有布局都变了，改一个 XML 就重编全部——「每个布局
// 独立编译单元」的意义随之消失。
bool writeFile(const std::string& path, const std::string& contents, std::string& why) {
  std::string existing;
  if (readFile(path, existing) && existing == contents) return true;

  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    why = "写不进 " + path;
    return false;
  }
  const size_t written = std::fwrite(contents.data(), 1, contents.size(), file);
  std::fclose(file);
  if (written != contents.size()) {
    why = "写入不完整：" + path;
    return false;
  }
  return true;
}

std::string stemOf(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos) name = name.substr(0, dot);
  return name;
}

struct Emitted {
  std::string stem;
  std::string function;
};

}  // namespace

int main(int argc, char** argv) {
  std::string outDir;
  std::vector<std::string> inputs;
  // --registry-fn NAME：额外生成一个显式注册函数。
  //
  // 静态初始化期的注册项在静态库里有被链接器整块丢弃的风险（没有人引用
  // 那个目标文件）。内置布局（标题栏）编译进 pyui_core，而 pyui_core 是
  // 静态库，所以必须提供一个**被显式调用**的入口：宿主启动时调一次
  // NAME()，才保证这些布局一定在注册表里。
  std::string registryFunction;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "pyui_layoutc: --out 需要一个目录\n");
        return 2;
      }
      outDir = argv[++i];
    } else if (arg == "--registry-fn") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "pyui_layoutc: --registry-fn 需要一个函数名\n");
        return 2;
      }
      registryFunction = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::printf("用法：pyui_layoutc --out <生成目录> [--registry-fn <名字>] <布局 XML>...\n");
      return 0;
    } else {
      inputs.push_back(arg);
    }
  }

  if (outDir.empty() || inputs.empty()) {
    std::fprintf(stderr, "用法：pyui_layoutc --out <生成目录> <布局 XML>...\n");
    return 2;
  }

  std::vector<Diagnostic> diagnostics;
  std::vector<Emitted> emitted;
  std::string ioError;

  for (const auto& input : inputs) {
    const xml::Document doc = xml::parseFile(input);
    if (!doc.ok) {
      diagnostics.push_back(
          Diagnostic{input, doc.error.line, doc.error.column, doc.error.message});
      continue;
    }

    Context ctx;
    ctx.file = input;
    ctx.sink = &diagnostics;

    Generator generator(ctx, doc.root);
    std::string body;
    std::string rootVar;
    if (!generator.emit(body, rootVar)) continue;  // 错误已进 diagnostics
    if (ctx.errors > 0) continue;

    const std::string stem = stemOf(input);
    const std::string function = "create" + identFromStem(stem);

    std::string source;
    source += "// 由 pyui_layoutc 从 " + stem + ".xml 生成，请勿手工修改\n";
    source += "#include \"pyui/layout_inflater.h\"\n";
    source += "#include \"pyui/layouts/frame_layout.h\"\n";
    source += "#include \"pyui/layouts/linear_layout.h\"\n";
    source += "#include \"pyui/widgets/button.h\"\n";
    source += "#include \"pyui/widgets/check_box.h\"\n";
    source += "#include \"pyui/widgets/image_view.h\"\n";
    source += "#include \"pyui/widgets/progress_bar.h\"\n";
    source += "#include \"pyui/widgets/radio_button.h\"\n";
    source += "#include \"pyui/widgets/slider.h\"\n";
    source += "#include \"pyui/widgets/text_field.h\"\n";
    source += "#include \"pyui/widgets/text_view.h\"\n";
    source += "\nnamespace pyui {\nnamespace generated {\n\n";
    source += "ViewGroup* " + function + "() {\n";
    source += body;
    source += "  return " + rootVar + ";\n";
    source += "}\n\n";
    source += "}  // namespace generated\n}  // namespace pyui\n";

    const std::string path = outDir + "/" + stem + ".gen.cpp";
    if (!writeFile(path, source, ioError)) {
      std::fprintf(stderr, "pyui_layoutc: %s\n", ioError.c_str());
      return 1;
    }
    emitted.push_back(Emitted{stem, function});
  }

  if (!diagnostics.empty()) {
    for (const auto& d : diagnostics) {
      std::fprintf(stderr, "%s:%d:%d: error: %s\n", d.file.c_str(), d.line, d.column,
                   d.message.c_str());
    }
    std::fprintf(stderr, "pyui_layoutc: %zu 个错误\n", diagnostics.size());
    return 1;
  }

  // 汇总注册表。它被应用直接编译，因此引用的工厂不会被链接器丢掉。
  //
  // --registry-fn 时会额外出一个**显式调用**的函数：静态初始化期的注册项
  // 在静态库（pyui_core 就是）里有被整块丢弃的风险，内置布局必须保证
  // 一定在表里，所以给宿主一个启动时调用的入口。
  std::string registry;
  registry += "// 由 pyui_layoutc 生成，请勿手工修改\n";
  registry += "#include \"pyui/layout_inflater.h\"\n\n";
  registry += "namespace pyui {\nnamespace generated {\n\n";
  for (const auto& item : emitted) {
    registry += "ViewGroup* " + item.function + "();\n";
  }
  registry += "\n}  // namespace generated\n\nnamespace {\n\n";
  for (size_t i = 0; i < emitted.size(); ++i) {
    registry += "const LayoutRegistration kLayoutRegistration" + std::to_string(i) + "(" +
                cppString(emitted[i].stem) + ", &generated::" + emitted[i].function + ");\n";
  }
  registry += "\n}  // namespace\n";

  if (!registryFunction.empty()) {
    // 函数放在 pyui 命名空间下，声明进 layout_inflater.h 由调用方自行 include。
    // 用 static 局部量保证重复调用是幂等的：注册表是 map，重复写同一个键
    // 只是覆盖成同一个工厂，没有副作用，但幂等能省掉调用方的状态位。
    registry += "\nvoid " + registryFunction + "() {\n";
    registry += "  static const bool once = [] {\n";
    for (const auto& item : emitted) {
      registry += "    LayoutInflater::add(" + cppString(item.stem) + ", &generated::" +
                  item.function + ");\n";
    }
    registry += "    return true;\n";
    registry += "  }();\n";
    registry += "  (void)once;\n";
    registry += "}\n";
  }
  registry += "\n}  // namespace pyui\n";

  if (!writeFile(outDir + "/pyui_layouts.gen.cpp", registry, ioError)) {
    std::fprintf(stderr, "pyui_layoutc: %s\n", ioError.c_str());
    return 1;
  }

  for (const auto& item : emitted) {
    std::printf("pyui_layoutc: %s -> %s.gen.cpp\n", item.stem.c_str(), item.stem.c_str());
  }
  return 0;
}
