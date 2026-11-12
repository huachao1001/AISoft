// layoutgen：把 XML 布局编译成 C++ 代码
//
// 用法：layoutgen <输入.xml> <输出.h> <输出.cpp>
//
// 生成的类继承根节点对应的布局容器（LinearLayout / FrameLayout），构造
// 函数里按 XML 建出全部子控件；带 id 的控件生成为公开成员，宿主直接取用。
//
// 支持的标签：
//   <LinearLayout orientation="vertical|horizontal" spacing="8" padding="8">
//   <FrameLayout padding="8">
//   <Button id="refresh" text="刷新" icon="refresh" style="outline" width="96" height="32"/>
//   <TextView id="title" text="名称" textColor="#333333" align="left" bold="true"/>
//   <ImageView id="logo" src="logo.png" scale="fit" width="24" height="24"/>
//   <InputBox id="search" cue="搜索" icon="search" radius="7"/>
//
// 通用属性：width / height（数字、wrap、match）、weight、margin /
// marginX / marginY、x / y、align / alignY。
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ── 极简 XML 解析：只处理布局需要的子集 ──

struct XmlNode {
  std::string tag;
  std::map<std::string, std::string> attributes;
  std::vector<XmlNode> children;
  std::string text;
};

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// 解码常见实体
std::string decodeEntities(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '&') {
      out.push_back(in[i]);
      continue;
    }
    const size_t semi = in.find(';', i);
    if (semi == std::string::npos) {
      out.push_back(in[i]);
      continue;
    }
    const std::string entity = in.substr(i + 1, semi - i - 1);
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
    } else {
      out.append(in, i, semi - i + 1);
    }
    i = semi;
  }
  return out;
}

class XmlParser {
 public:
  explicit XmlParser(std::string source) : source_(std::move(source)) {}

  bool parse(XmlNode& root, std::string& error) {
    skipMisc();
    if (!parseNode(root)) {
      error = error_;
      return false;
    }
    skipMisc();
    if (pos_ < source_.size()) {
      error = "根元素之后存在多余内容";
      return false;
    }
    return true;
  }

  int line() const {
    int n = 1;
    for (size_t i = 0; i < pos_ && i < source_.size(); ++i) {
      if (source_[i] == '\n') ++n;
    }
    return n;
  }

 private:
  // 跳过声明、注释与空白
  void skipMisc() {
    for (;;) {
      skipSpace();
      if (pos_ + 1 >= source_.size()) return;
      if (source_[pos_] == '<' && source_[pos_ + 1] == '?') {
        const size_t end = source_.find("?>", pos_);
        if (end == std::string::npos) return;
        pos_ = end + 2;
      } else if (source_.compare(pos_, 4, "<!--") == 0) {
        const size_t end = source_.find("-->", pos_);
        if (end == std::string::npos) return;
        pos_ = end + 3;
      } else {
        return;
      }
    }
  }

  void skipSpace() {
    while (pos_ < source_.size() && isSpace(source_[pos_])) ++pos_;
  }

  bool fail(const std::string& message) {
    std::ostringstream out;
    out << "第 " << line() << " 行：" << message;
    error_ = out.str();
    return false;
  }

  bool parseNode(XmlNode& node) {
    skipMisc();
    if (pos_ >= source_.size() || source_[pos_] != '<') return fail("期望元素开始");
    ++pos_;

    const size_t nameStart = pos_;
    while (pos_ < source_.size() && !isSpace(source_[pos_]) && source_[pos_] != '>' &&
           source_[pos_] != '/') {
      ++pos_;
    }
    node.tag = source_.substr(nameStart, pos_ - nameStart);
    if (node.tag.empty()) return fail("元素名为空");

    // 属性
    for (;;) {
      skipSpace();
      if (pos_ >= source_.size()) return fail("元素未闭合：" + node.tag);
      if (source_[pos_] == '/') {
        if (pos_ + 1 < source_.size() && source_[pos_ + 1] == '>') {
          pos_ += 2;
          return true;
        }
        ++pos_;
        continue;
      }
      if (source_[pos_] == '>') {
        ++pos_;
        break;
      }

      const size_t keyStart = pos_;
      while (pos_ < source_.size() && source_[pos_] != '=' && !isSpace(source_[pos_]) &&
             source_[pos_] != '>') {
        ++pos_;
      }
      const std::string key = source_.substr(keyStart, pos_ - keyStart);
      skipSpace();
      if (pos_ >= source_.size() || source_[pos_] != '=') return fail("属性缺少等号：" + key);
      ++pos_;
      skipSpace();
      if (pos_ >= source_.size() || (source_[pos_] != '"' && source_[pos_] != '\'')) {
        return fail("属性值缺少引号：" + key);
      }
      const char quote = source_[pos_++];
      const size_t valueStart = pos_;
      while (pos_ < source_.size() && source_[pos_] != quote) ++pos_;
      if (pos_ >= source_.size()) return fail("属性值未闭合：" + key);
      node.attributes[key] = decodeEntities(source_.substr(valueStart, pos_ - valueStart));
      ++pos_;
    }

    // 子节点与文本
    for (;;) {
      if (pos_ >= source_.size()) return fail("元素未闭合：" + node.tag);
      if (source_.compare(pos_, 2, "</") == 0) {
        const size_t end = source_.find('>', pos_);
        if (end == std::string::npos) return fail("结束标签未闭合：" + node.tag);
        const std::string closing =
            source_.substr(pos_ + 2, end - pos_ - 2);
        if (closing != node.tag) return fail("结束标签不匹配：" + closing + "，期望 " + node.tag);
        pos_ = end + 1;
        return true;
      }
      if (source_.compare(pos_, 4, "<!--") == 0) {
        const size_t end = source_.find("-->", pos_);
        pos_ = (end == std::string::npos) ? source_.size() : end + 3;
        continue;
      }
      if (source_[pos_] == '<') {
        XmlNode child;
        if (!parseNode(child)) return false;
        node.children.push_back(std::move(child));
        continue;
      }
      const size_t textStart = pos_;
      while (pos_ < source_.size() && source_[pos_] != '<') ++pos_;
      node.text += decodeEntities(source_.substr(textStart, pos_ - textStart));
    }
  }

  std::string source_;
  size_t pos_ = 0;
  std::string error_;
};

// ── 属性读取 ──

bool has(const XmlNode& node, const char* key) { return node.attributes.count(key) > 0; }

std::string get(const XmlNode& node, const char* key, const std::string& fallback = "") {
  auto it = node.attributes.find(key);
  return it == node.attributes.end() ? fallback : it->second;
}

int getInt(const XmlNode& node, const char* key, int fallback) {
  const std::string value = get(node, key);
  if (value.empty()) return fallback;
  return std::atoi(value.c_str());
}

float getFloat(const XmlNode& node, const char* key, float fallback) {
  const std::string value = get(node, key);
  if (value.empty()) return fallback;
  return static_cast<float>(std::atof(value.c_str()));
}

bool getBool(const XmlNode& node, const char* key, bool fallback) {
  const std::string value = get(node, key);
  if (value.empty()) return fallback;
  return value == "true" || value == "1" || value == "yes";
}

// 尺寸：数字为设计值，wrap 按内容，match 撑满
std::string sizeExpr(const XmlNode& node, const char* key) {
  const std::string value = get(node, key);
  if (value == "wrap" || value == "content") return "LayoutParams::kWrapContent";
  if (value == "match" || value == "parent") return "LayoutParams::kMatchParent";
  if (!value.empty()) return value;
  // 未写尺寸时宽度撑满、高度按内容。反过来的话纵向容器里每个子视图
  // 都会独占整高，后面的视图被挤出可见区域。
  return std::string(key) == "height" ? "LayoutParams::kWrapContent"
                                     : "LayoutParams::kMatchParent";
}

// 宽字符字面量：源文件统一 UTF-8，编译时由 /utf-8 转成 UTF-16
std::string wideLiteral(const std::string& utf8) {
  std::string out = "L\"";
  for (size_t i = 0; i < utf8.size(); ++i) {
    const char c = utf8[i];
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      continue;
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out.push_back(c);
    }
  }
  out += "\"";
  return out;
}

std::string trimmed(const std::string& text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return std::string();
  const size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

// 图标名到枚举
std::string iconExpr(const std::string& name) {
  static const std::map<std::string, std::string> kIcons = {
      {"refresh", "IconId::kRefresh"},        {"extract", "IconId::kExtract"},
      {"extractAll", "IconId::kExtractAll"},  {"add", "IconId::kAdd"},
      {"trash", "IconId::kTrash"},            {"arrowBack", "IconId::kArrowBack"},
      {"chevron", "IconId::kChevron"},        {"chevronUp", "IconId::kChevronUp"},
      {"chevronDown", "IconId::kChevronDown"},{"home", "IconId::kHome"},
      {"folder", "IconId::kFolder"},          {"file", "IconId::kFile"},
      {"search", "IconId::kSearch"},
  };
  auto it = kIcons.find(name);
  return it == kIcons.end() ? "IconId::kNone" : it->second;
}

// "#rrggbb" / "#aarrggbb" 转 makeColor 表达式
std::string colorExpr(const XmlNode& node, const char* key, const std::string& fallback) {
  const std::string value = get(node, key);
  if (value.empty() || value[0] != '#') return fallback;
  const std::string hex = value.substr(1);
  auto parse = [](const std::string& s, size_t offset) {
    return static_cast<int>(std::strtol(s.substr(offset, 2).c_str(), nullptr, 16));
  };
  int r = 0, g = 0, b = 0, a = 255;
  if (hex.size() == 6) {
    r = parse(hex, 0);
    g = parse(hex, 2);
    b = parse(hex, 4);
  } else if (hex.size() == 8) {
    a = parse(hex, 0);
    r = parse(hex, 2);
    g = parse(hex, 4);
    b = parse(hex, 6);
  } else {
    return fallback;
  }
  char buffer[80];
  std::snprintf(buffer, sizeof(buffer), "makeColor(0x%02x, 0x%02x, 0x%02x%s)", r, g, b,
                a == 255 ? "" : (", 0x" + std::to_string(a)).c_str());
  return buffer;
}

std::string alignExpr(const std::string& value) {
  if (value == "center") return "TextAlign::kCenter";
  if (value == "right") return "TextAlign::kRight";
  return "TextAlign::kLeft";
}

std::string gravityExpr(const std::string& value) {
  if (value == "center") return "Gravity::kCenter";
  if (value == "end") return "Gravity::kEnd";
  return "Gravity::kStart";
}

std::string scaleExpr(const std::string& value) {
  if (value == "fill") return "ImageScale::kFill";
  if (value == "stretch") return "ImageScale::kStretch";
  if (value == "center") return "ImageScale::kCenter";
  return "ImageScale::kFit";
}

std::string orientationExpr(const std::string& value) {
  return value == "horizontal" ? "Orientation::kHorizontal" : "Orientation::kVertical";
}

bool isValidIdentifier(const std::string& name) {
  if (name.empty()) return false;
  if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
  for (char c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  }
  return true;
}

// 内建标签 → 头文件
std::string headerFor(const std::string& tag) {
  static const std::map<std::string, std::string> kHeaders = {
      {"LinearLayout", "AiUI/layouts/linear_layout.h"},
      {"FrameLayout", "AiUI/layouts/frame_layout.h"},
      {"View", "AiUI/view.h"},
      {"Button", "AiUI/widgets/button.h"},
      {"TextView", "AiUI/widgets/text_view.h"},
      {"ImageView", "AiUI/widgets/image_view.h"},
      {"InputBox", "AiUI/widgets/input_box.h"},
      {"TreeView", "AiUI/widgets/tree_view.h"},
      {"ListView", "AiUI/widgets/list_view.h"},
      {"BreadcrumbBar", "AiUI/widgets/breadcrumb_bar.h"},
      {"StatusBar", "AiUI/widgets/status_bar.h"},
      {"ButtonBar", "AiUI/widgets/button_bar.h"},
      {"CheckBox", "AiUI/widgets/option_view.h"},
      {"RadioButton", "AiUI/widgets/option_view.h"},
      {"RadioGroup", "AiUI/widgets/form_controls.h"},
      {"ProgressBar", "AiUI/widgets/form_controls.h"},
      {"GroupBox", "AiUI/widgets/form_controls.h"},
      {"ComboBox", "AiUI/widgets/form_controls.h"},
  };
  auto it = kHeaders.find(tag);
  return it == kHeaders.end() ? std::string() : it->second;
}

// 节点对应的 C++ 类型就是标签名。非内建控件靠 header 属性声明所在头文件、
// 靠 ns 属性声明命名空间：
//   <CrumbBar id="crumb" ns="ui" header="ui_crumb.h" height="48"/>
std::string typeOf(const XmlNode& node) {
  const std::string ns = get(node, "ns");
  return ns.empty() ? node.tag : "::" + ns + "::" + node.tag;
}

// 标签是否可识别：内建控件，或带 header 属性的自定义控件
bool isKnown(const XmlNode& node) {
  return !headerFor(node.tag).empty() || has(node, "header");
}

bool isContainer(const std::string& tag) {
  return tag == "LinearLayout" || tag == "FrameLayout" || tag == "RadioGroup" ||
         tag == "GroupBox";
}

// ── 代码生成 ──

class Generator {
 public:
  explicit Generator(std::string className) : className_(std::move(className)) {}

  bool generate(const XmlNode& root, std::string& error) {
    if (!isContainer(root.tag)) {
      error = "根节点必须是 LinearLayout 或 FrameLayout";
      return false;
    }
    if (headerFor(root.tag).empty()) {
      error = "未知标签：" + root.tag;
      return false;
    }

    rootTag_ = root.tag;
    include(root.tag);
    // 生成的代码用 makeId 给带 id 的控件打标，统一在这里引入
    addInclude("AiUI/id.h");

    // 根节点属性作用在 this 上
    emitContainerProperties(root, "this");
    // 根节点自身的 id 也要能查到；不需要额外成员，this 就是它
    const std::string rootId = get(root, "id");
    if (isValidIdentifier(rootId)) line(1, "this->setId(AiUI::makeId(\"" + rootId + "\"));");
    // 先扫描全部 id，保证成员声明顺序与 XML 一致
    for (const XmlNode& child : root.children) {
      if (!emitNode(child, "this")) {
        error = error_;
        return false;
      }
    }
    return true;
  }

  std::string headerFile() const {
    std::ostringstream out;
    out << "// 由 layoutgen 从 XML 生成，请勿手工修改\n";
    out << "#pragma once\n\n";
    for (const std::string& path : includes_) {
      out << "#include \"" << path << "\"\n";
    }
    out << "\nnamespace AiUI {\nnamespace generated {\n\n";
    out << "class " << className_ << " : public " << rootTag_ << " {\n";
    out << " public:\n";
    out << "  " << className_ << "();\n";
    if (!members_.empty()) {
      out << "\n";
      for (const Member& member : members_) {
        out << "  " << member.type << "* " << member.name << " = nullptr;\n";
      }
    }
    out << "};\n\n";
    out << "}  // namespace generated\n}  // namespace AiUI\n";
    return out.str();
  }

  std::string sourceFile(const std::string& headerName) const {
    std::ostringstream out;
    out << "// 由 layoutgen 从 XML 生成，请勿手工修改\n";
    out << "#include \"" << headerName << "\"\n\n";
    out << "namespace AiUI {\nnamespace generated {\n\n";
    out << className_ << "::" << className_ << "() {\n";
    out << body_.str();
    out << "}\n\n";
    out << "}  // namespace generated\n}  // namespace AiUI\n";
    return out.str();
  }

 private:
  struct Member {
    std::string type;
    std::string name;
  };

  void include(const std::string& tag) {
    const std::string path = headerFor(tag);
    if (!path.empty()) addInclude(path);
  }

  void addInclude(const std::string& path) {
    if (path.empty()) return;
    for (const std::string& existing : includes_) {
      if (existing == path) return;
    }
    includes_.push_back(path);
  }

  void line(int indent, const std::string& text) {
    body_ << std::string(static_cast<size_t>(indent) * 2, ' ') << text << "\n";
  }

  // 通用布局参数表达式
  std::string layoutParams(const XmlNode& node) {
    std::ostringstream out;
    out << "LayoutParams{}.withSize(" << sizeExpr(node, "width") << ", "
        << sizeExpr(node, "height") << ")";

    const int margin = getInt(node, "margin", 0);
    const int marginH = getInt(node, "marginX", margin);
    const int marginV = getInt(node, "marginY", margin);
    if (marginH != 0 || marginV != 0) out << ".margin(" << marginH << ", " << marginV << ")";

    const float weight = getFloat(node, "weight", 0.0f);
    if (weight > 0.0f) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.2ff", weight);
      out << ".withWeight(" << buffer << ")";
    }

    if (has(node, "align") || has(node, "alignY")) {
      out << ".align(" << gravityExpr(get(node, "align", "start")) << ", "
          << gravityExpr(get(node, "alignY", "start")) << ")";
    }
    if (has(node, "x") || has(node, "y")) {
      out << ".offset(" << getInt(node, "x", 0) << ", " << getInt(node, "y", 0) << ")";
    }
    return out.str();
  }

  // 容器自身的属性
  void emitContainerProperties(const XmlNode& node, const std::string& target) {
    const std::string tag = node.tag;
    if (tag == "LinearLayout") {
      const std::string orientation = get(node, "orientation", "vertical");
      line(1, target + "->setOrientation(" + orientationExpr(orientation) + ");");
      if (has(node, "spacing")) {
        line(1, target + "->setSpacing(" + get(node, "spacing", "0") + ");");
      }
      if (has(node, "gravity")) {
        line(1, target + "->setGravity(" + gravityExpr(get(node, "gravity")) + ");");
      }
    }
    if (tag == "GroupBox") {
      const std::string title = get(node, "title");
      if (!title.empty()) line(1, target + "->setTitle(" + wideLiteral(title) + ");");
      if (has(node, "titleHeight")) {
        line(1, target + "->setTitleHeight(" + get(node, "titleHeight", "26") + ");");
      }
    }
    if (tag == "RadioGroup" && has(node, "rowGap")) {
      line(1, target + "->setRowGap(" + get(node, "rowGap", "8") + ");");
    }
    if (tag == "RadioGroup" && has(node, "orientation")) {
      line(1, target + "->setOrientation(" + orientationExpr(get(node, "orientation", "vertical")) +
                     ");");
    }
    if (has(node, "padding")) {
      line(1, target + "->setPadding(" + get(node, "padding", "0") + ");");
    }
  }

  // 生成一个节点及其子树
  bool emitNode(const XmlNode& node, const std::string& parentVar) {
    if (!isKnown(node)) {
      error_ = "未知标签：" + node.tag + "（自定义控件需用 header 属性指明头文件）";
      return false;
    }
    include(node.tag);
    if (has(node, "header")) addInclude(get(node, "header"));

    const std::string id = get(node, "id");
    // 匿名控件也要有合法变量名；生成类只把带 id 的暴露成成员
    const bool named = isValidIdentifier(id);
    std::string variable = named ? id : "_widget" + std::to_string(++anonymous_);
    const std::string type = typeOf(node);

    line(1, named ? (variable + " = new " + type + "();")
                  : ("auto* " + variable + " = new " + type + "();"));
    // id 要在挂树前设好：宿主可能在 onCreated 里就查它
    if (named) line(1, variable + "->setId(AiUI::makeId(\"" + id + "\"));");
    // 布局容器与普通容器都走 ViewGroup::addView，布局参数由它转交
    line(1, parentVar + "->addView(" + variable + ", " + layoutParams(node) + ");");

    // 任意节点都可以直接铺底色：对话框的底色就是这么给的
    if (has(node, "background")) {
      line(1, variable + "->setBackgroundColor(" + colorExpr(node, "background", "kNoColor") +
                     ");");
    }

    if (isContainer(node.tag)) emitContainerProperties(node, variable);
    else emitWidgetProperties(node, variable);

    if (isContainer(node.tag)) {
      for (const XmlNode& child : node.children) {
        if (!emitNode(child, variable)) return false;
      }
    }

    // 有 id 的控件暴露给宿主
    if (named) members_.push_back(Member{type, variable});
    return true;
  }

  void emitWidgetProperties(const XmlNode& node, const std::string& variable) {
    const std::string& tag = node.tag;

    if (tag == "Button") {
      line(1, "{");
      line(2, "ButtonSpec spec;");
      const std::string text = get(node, "text");
      if (!text.empty()) line(2, "spec.label = " + wideLiteral(text) + ";");
      const std::string icon = get(node, "icon");
      if (!icon.empty()) {
        line(2, "spec.icon = " + iconExpr(icon) + ";");
        line(2, "spec.hasIcon = true;");
      }
      const std::string placement = get(node, "iconPlacement");
      if (placement == "top") line(2, "spec.placement = IconPlacement::kTop;");
      else if (placement == "only") line(2, "spec.placement = IconPlacement::kOnly;");
      else if (placement == "none") line(2, "spec.placement = IconPlacement::kNone;");

      const std::string style = get(node, "style");
      if (style == "subtle") line(2, "spec.style = ButtonStyle::kSubtle;");
      else if (style == "flat") line(2, "spec.style = ButtonStyle::kFlat;");

      if (has(node, "width")) line(2, "spec.width = " + get(node, "width", "0") + ";");
      if (has(node, "height")) line(2, "spec.height = " + get(node, "height", "32") + ";");
      if (has(node, "iconSize")) line(2, "spec.iconSize = " + get(node, "iconSize", "18") + ";");
      if (has(node, "radius")) line(2, "spec.radius = " + get(node, "radius", "7") + ";");
      if (has(node, "padX")) line(2, "spec.padX = " + get(node, "padX", "12") + ";");
      if (has(node, "iconGap")) line(2, "spec.iconGap = " + get(node, "iconGap", "6") + ";");
      if (has(node, "textColor")) {
        line(2, "spec.ink = " + colorExpr(node, "textColor", "kNoColor") + ";");
      }
      if (has(node, "backgroundColor")) {
        line(2, "spec.background = " + colorExpr(node, "backgroundColor", "kNoColor") + ";");
      }
      if (has(node, "borderColor")) {
        line(2, "spec.border = " + colorExpr(node, "borderColor", "kNoColor") + ";");
      }
      line(2, variable + "->setSpec(spec);");
      line(1, "}");
      return;
    }

    if (tag == "TextView") {
      std::string text = get(node, "text");
      if (text.empty()) text = trimmed(node.text);
      if (!text.empty()) line(1, variable + "->setText(" + wideLiteral(text) + ");");

      line(1, "{");
      line(2, "TextStyle style;");
      if (has(node, "textColor")) {
        line(2, "style.color = " + colorExpr(node, "textColor", "kColorText") + ";");
      }
      if (has(node, "align")) line(2, "style.align = " + alignExpr(get(node, "align")) + ";");
      if (getBool(node, "bold", false)) line(2, "style.bold = true;");
      if (getBool(node, "small", false)) line(2, "style.smallFont = true;");
      if (has(node, "ellipsis") && !getBool(node, "ellipsis", true)) {
        line(2, "style.ellipsis = false;");
      }
      if (has(node, "padding")) {
        line(2, "style.paddingLeft = style.paddingTop = style.paddingRight = "
                "style.paddingBottom = " +
                    get(node, "padding", "0") + ";");
      }
      if (has(node, "paddingX")) {
        line(2, "style.paddingLeft = style.paddingRight = " + get(node, "paddingX", "0") + ";");
      }
      line(2, variable + "->setTextStyle(style);");
      line(1, "}");
      return;
    }

    if (tag == "ImageView") {
      const std::string source = get(node, "src");
      if (!source.empty()) line(1, variable + "->loadFile(" + wideLiteral(source) + ");");
      const std::string icon = get(node, "icon");
      if (!icon.empty()) line(1, variable + "->setIcon(" + iconExpr(icon) + ");");
      if (has(node, "scale")) line(1, variable + "->setScale(" + scaleExpr(get(node, "scale")) + ");");
      if (has(node, "tint")) {
        line(1, variable + "->setPlaceholderColor(" + colorExpr(node, "tint", "kColorTextFaint") +
                       ");");
      }
      return;
    }

    if (tag == "InputBox") {
      line(1, "{");
      line(2, "InputSpec spec;");
      const std::string cue = get(node, "cue");
      if (!cue.empty()) line(2, "spec.cue = " + wideLiteral(cue) + ";");
      const std::string icon = get(node, "icon");
      if (!icon.empty()) {
        line(2, "spec.hasLeadIcon = true;");
        line(2, "spec.leadIcon = " + iconExpr(icon) + ";");
      }
      if (has(node, "radius")) line(2, "spec.radius = " + get(node, "radius", "7") + ";");
      if (has(node, "padding")) line(2, "spec.padX = " + get(node, "padding", "11") + ";");
      if (has(node, "outerColor")) {
        line(2, "spec.outerBackground = " + colorExpr(node, "outerColor", "kColorPane") + ";");
      }
      line(2, variable + "->setSpec(spec);");
      line(1, "}");
      return;
    }

    // 选项行：CheckBox / RadioButton 共用一份配置
    if (tag == "CheckBox" || tag == "RadioButton") {
      line(1, "{");
      line(2, "OptionSpec spec;");
      // 文字可以直接写在标签内，也可以放 text 属性
      std::string text = get(node, "text");
      if (text.empty()) text = trimmed(node.text);
      if (!text.empty()) line(2, "spec.label = " + wideLiteral(text) + ";");
      if (getBool(node, "checked", false)) line(2, "spec.checked = true;");
      if (has(node, "boxSize")) line(2, "spec.boxSize = " + get(node, "boxSize", "16") + ";");
      if (has(node, "gap")) line(2, "spec.gap = " + get(node, "gap", "8") + ";");
      if (has(node, "textColor")) {
        line(2, "spec.ink = " + colorExpr(node, "textColor", "kColorText") + ";");
      }
      line(2, variable + "->setSpec(spec);");
      line(1, "}");
      return;
    }

    if (tag == "ProgressBar") {
      line(1, "{");
      line(2, "ProgressSpec spec;");
      if (has(node, "barHeight")) line(2, "spec.height = " + get(node, "barHeight", "10") + ";");
      if (has(node, "radius")) line(2, "spec.radius = " + get(node, "radius", "5") + ";");
      if (has(node, "fillColor")) {
        line(2, "spec.fill = " + colorExpr(node, "fillColor", "kColorLink") + ";");
      }
      if (has(node, "trackColor")) {
        line(2, "spec.track = " + colorExpr(node, "trackColor", "kColorLineSoft") + ";");
      }
      line(2, variable + "->setSpec(spec);");
      line(1, "}");
      if (has(node, "progress")) {
        line(1, variable + "->setProgress(" + get(node, "progress", "0") + "f);");
      }
      return;
    }

    if (tag == "ComboBox") {
      line(1, "{");
      line(2, "ComboSpec spec;");
      if (has(node, "height")) line(2, "spec.height = " + get(node, "height", "30") + ";");
      if (has(node, "radius")) line(2, "spec.radius = " + get(node, "radius", "6") + ";");
      if (has(node, "padding")) line(2, "spec.padX = " + get(node, "padding", "10") + ";");
      line(2, variable + "->setSpec(spec);");
      line(1, "}");
      // 候选项用分号分隔：items="仅压缩;快速;极限"
      const std::string items = get(node, "items");
      if (!items.empty()) {
        std::vector<std::string> parts;
        std::string current;
        for (char c : items) {
          if (c == ';') {
            parts.push_back(current);
            current.clear();
          } else {
            current.push_back(c);
          }
        }
        parts.push_back(current);
        line(1, "{");
        line(2, "std::vector<std::wstring> items;");
        for (const auto& part : parts) line(2, "items.push_back(" + wideLiteral(part) + ");");
        line(2, variable + "->setItems(items);");
        line(1, "}");
      }
      if (has(node, "selected")) {
        line(1, variable + "->setSelectedIndex(" + get(node, "selected", "0") + ", false);");
      }
      return;
    }
  }

  std::string className_;
  std::string rootTag_;
  std::vector<std::string> includes_;
  std::vector<Member> members_;
  std::ostringstream body_;
  std::string error_;
  int anonymous_ = 0;
};

// 从路径取不含扩展名的文件名，转成合法的类名
std::string classFromPath(const std::string& path) {
  size_t begin = path.find_last_of("\\/");
  begin = (begin == std::string::npos) ? 0 : begin + 1;
  size_t end = path.find_last_of('.');
  if (end == std::string::npos || end < begin) end = path.size();
  std::string stem = path.substr(begin, end - begin);

  std::string name;
  bool upper = true;
  for (char c : stem) {
    if (!std::isalnum(static_cast<unsigned char>(c))) {
      upper = true;
      continue;
    }
    name.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
    upper = false;
  }
  if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) name = "Layout" + name;
  return name;
}

// 从路径取路径末段（不含扩展名），作为 LayoutInflater 的名字
std::string stemFromPath(const std::string& path) {
  size_t begin = path.find_last_of("\\/");
  begin = (begin == std::string::npos) ? 0 : begin + 1;
  size_t end = path.find_last_of('.');
  if (end == std::string::npos || end < begin) end = path.size();
  return path.substr(begin, end - begin);
}

// 生成注册文件：把每个 XML 的类名登记进 LayoutInflater，名字用文件名。
// 这样宿主按 XML 名字取布局，实际调用的是编译期生成好的类。
std::string registrySource(const std::vector<std::string>& xmlPaths) {
  std::ostringstream out;
  out << "// 由 layoutgen 自动生成，请勿手工修改\n";
  out << "#include \"AiUI/layout_inflater.h\"\n\n";
  for (const std::string& path : xmlPaths) {
    out << "#include \"" << stemFromPath(path) << ".h\"\n";
  }
  out << "\nnamespace AiUI {\nnamespace generated {\n\n";
  for (const std::string& path : xmlPaths) {
    const std::string cls = classFromPath(path);
    out << "namespace {\n";
    out << "AiUI::ViewGroup* create" << cls << "() { return new " << cls << "(); }\n";
    out << "const AiUI::LayoutRegistration kReg" << cls << "(L\"" << stemFromPath(path)
        << "\", &create" << cls << ");\n";
    out << "}  // namespace\n\n";
  }
  out << "}  // namespace generated\n}  // namespace AiUI\n";
  return out.str();
}

bool readFile(const char* path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  out = buffer.str();
  // 去掉 UTF-8 BOM
  if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
      static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
    out.erase(0, 3);
  }
  return true;
}

bool writeFile(const char* path, const std::string& content) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return out.good();
}

}  // namespace

int main(int argc, char** argv) {
  // 注册表模式：layoutgen --registry <输出.cpp> <file.xml> [<file.xml> ...]
  if (argc >= 3 && std::string(argv[1]) == "--registry") {
    std::vector<std::string> xmlPaths;
    for (int i = 3; i < argc; ++i) xmlPaths.push_back(argv[i]);
    if (!writeFile(argv[2], registrySource(xmlPaths))) {
      std::fprintf(stderr, "layoutgen: 无法写入 %s\n", argv[2]);
      return 1;
    }
    return 0;
  }

  if (argc != 4) {
    std::fprintf(stderr,
                 "用法：layoutgen <输入.xml> <输出.h> <输出.cpp>\n"
                 "      layoutgen --registry <输出.cpp> <file.xml> [<file.xml> ...]\n");
    return 2;
  }

  std::string source;
  if (!readFile(argv[1], source)) {
    std::fprintf(stderr, "layoutgen: 无法读取 %s\n", argv[1]);
    return 1;
  }

  XmlNode root;
  std::string error;
  XmlParser parser(std::move(source));
  if (!parser.parse(root, error)) {
    std::fprintf(stderr, "layoutgen: %s: %s\n", argv[1], error.c_str());
    return 1;
  }

  const std::string className = classFromPath(argv[1]);
  Generator generator(className);
  if (!generator.generate(root, error)) {
    std::fprintf(stderr, "layoutgen: %s: %s\n", argv[1], error.c_str());
    return 1;
  }

  const std::string headerPath = argv[2];
  std::string headerName = headerPath;
  const size_t slash = headerName.find_last_of("\\/");
  if (slash != std::string::npos) headerName = headerName.substr(slash + 1);

  if (!writeFile(argv[2], generator.headerFile()) || !writeFile(argv[3], generator.sourceFile(headerName))) {
    std::fprintf(stderr, "layoutgen: 无法写入输出文件\n");
    return 1;
  }
  return 0;
}

