#include "pyui/specs.h"

#include <cstdlib>
#include <cstring>

namespace pyui {
namespace specs {
namespace {

AttributeSpec str(std::string name, std::string setter, std::string note = {}) {
  AttributeSpec s;
  s.name = std::move(name);
  s.type = ValueType::kString;
  s.setter = std::move(setter);
  s.note = std::move(note);
  return s;
}

AttributeSpec text(std::string name, std::string setter) {
  AttributeSpec s;
  s.name = std::move(name);
  s.type = ValueType::kText;
  s.setter = std::move(setter);
  return s;
}

AttributeSpec num(std::string name, std::string setter, ValueType type = ValueType::kInt,
                  std::string note = {}) {
  AttributeSpec s;
  s.name = std::move(name);
  s.type = type;
  s.setter = std::move(setter);
  s.note = std::move(note);
  return s;
}

AttributeSpec flag(std::string name, std::string setter) {
  AttributeSpec s;
  s.name = std::move(name);
  s.type = ValueType::kBool;
  s.setter = std::move(setter);
  return s;
}

AttributeSpec color(std::string name, std::string setter) {
  AttributeSpec s;
  s.name = std::move(name);
  s.type = ValueType::kColor;
  s.setter = std::move(setter);
  return s;
}

AttributeSpec enumOf(std::string name, std::string setter, std::string enumType,
                     std::vector<std::string> values, std::string note = {}) {
  AttributeSpec s;
  s.name = std::move(name);
  s.type = ValueType::kEnum;
  s.setter = std::move(setter);
  s.enumType = std::move(enumType);
  s.allowedValues = std::move(values);
  s.note = std::move(note);
  return s;
}

AttributeSpec design(std::string name, std::string setter) {
  AttributeSpec s;
  s.name = std::move(name);
  s.type = ValueType::kDesign;
  s.setter = std::move(setter);
  return s;
}

AttributeSpec layoutParam(std::string name, ValueType type, std::string note = {}) {
  AttributeSpec s;
  s.name = std::move(name);
  s.type = type;
  s.layoutParam = true;
  s.note = std::move(note);
  return s;
}

std::vector<AttributeSpec> buildCommon() {
  std::vector<AttributeSpec> out;
  {
    AttributeSpec s;
    s.name = "id";
    s.type = ValueType::kId;
    s.note = "视图 id，供 findView 按名字取";
    out.push_back(s);
  }
  out.push_back(flag("visible", "setVisible"));
  out.push_back(flag("enabled", "setEnabled"));
  out.push_back(color("background", "setBackgroundColor"));
  // 窗口角色：把「这个控件在窗口里干什么」写成布局数据，平台判定只认它。
  // 没有它就只能靠约定 id 认最大化按钮，按钮一挪位置或换个 id 就失灵。
  //
  // 它是**能力声明**而不是外观属性：写了 caption 就能拖窗口，写了
  // minimize/maximize/close 就真的会最小化/最大化/关闭，与放在哪个布局里、
  // 有没有内置标题栏无关。内置 titlebar.xml 只是这些声明的一份现成范例。
  out.push_back(enumOf("role", "setWindowRole", "WindowRole",
                       {"none", "caption", "maximize", "minimize", "close", "title", "icon"},
                       "窗口角色：caption 拖动，minimize/maximize/close 三个按钮，"
                       "title/icon 是装饰"));
  return out;
}

std::vector<AttributeSpec> buildContainer() {
  // 只放所有容器都有的：内边距。gravity / spacing 是 LinearLayout 独占的，
  // 放进公共表会让 FrameLayout 也「接受」它们，进而生成调不到 setter 的代码。
  return {
      design("padding", "setPadding"),
      design("paddingHorizontal", "setPaddingHorizontal"),
      design("paddingVertical", "setPaddingVertical"),
  };
}

std::vector<AttributeSpec> buildLayoutParams() {
  return {
      layoutParam("width", ValueType::kDimension, "match / wrap / 设计值"),
      layoutParam("height", ValueType::kDimension, "match / wrap / 设计值"),
      layoutParam("weight", ValueType::kFloat, "按权重分配剩余空间"),
      layoutParam("margin", ValueType::kDesign),
      layoutParam("marginLeft", ValueType::kDesign),
      layoutParam("marginTop", ValueType::kDesign),
      layoutParam("marginRight", ValueType::kDesign),
      layoutParam("marginBottom", ValueType::kDesign),
      layoutParam("align", ValueType::kString, "水平,垂直；取值 start/center/end"),
      layoutParam("x", ValueType::kDesign),
      layoutParam("y", ValueType::kDesign),
      layoutParam("maxWidth", ValueType::kDesign, "期望宽度上限；0 表示不限"),
      layoutParam("maxHeight", ValueType::kDesign, "期望高度上限；0 表示不限"),
  };
}

std::vector<WidgetSpec> buildAll() {
  std::vector<WidgetSpec> out;

  out.push_back(WidgetSpec{"View", "", false, {}});

  out.push_back(WidgetSpec{"LinearLayout", "ViewGroup", true,
                           {
                               enumOf("orientation", "setOrientation", "Orientation",
                                      {"horizontal", "vertical"}),
                               design("spacing", "setSpacing"),
                               enumOf("gravity", "setGravity", "Gravity",
                                      {"start", "center", "end"},
                                      "主轴上的整体对齐"),
                           }});

  out.push_back(WidgetSpec{"FrameLayout", "ViewGroup", true, {}});

  out.push_back(WidgetSpec{"TextView", "View", false,
                           {
                               text("text", "setText"),
                               enumOf("textSize", "setTextSize", "TextSize",
                                      {"small", "normal", "large", "huge"}),
                               flag("bold", "setBold"),
                               enumOf("textAlign", "setAlign", "TextAlign",
                                      {"left", "center", "right"}),
                               flag("ellipsis", "setEllipsis"),
                               flag("middleEllipsis", "setMiddleEllipsis"),
                               color("color", "setColor"),
                           }});

  out.push_back(WidgetSpec{"Button", "View", false,
                           {
                               text("text", "setLabel"),
                               enumOf("style", "setStyle", "ButtonStyle",
                                      {"outline", "subtle", "flat", "ghost", "primary"}),
                               enumOf("icon", "setIcon", "IconId",
                                      {"none", "check", "chevronDown", "chevronRight",
                                       "chevronUp", "minimize", "maximize", "restore",
                                       "close", "search", "folder", "file"}),
                               enumOf("iconPlacement", "setIconPlacement", "IconPlacement",
                                      {"none", "left", "top", "only"}),
                               design("iconSize", "setIconSize"),
                               design("padX", "setPadX"),
                               design("radius", "setRadius"),
                           }});

  out.push_back(WidgetSpec{"CheckBox", "View", false,
                           {
                               text("text", "setLabel"),
                               flag("checked", "setChecked"),
                               flag("tristate", "setTristate"),
                           }});

  out.push_back(WidgetSpec{"RadioButton", "View", false,
                           {
                               text("text", "setLabel"),
                               flag("checked", "setChecked"),
                           }});

  out.push_back(WidgetSpec{"TextField", "View", false,
                           {
                               text("text", "setText"),
                               text("placeholder", "setPlaceholder"),
                               flag("password", "setPassword"),
                               flag("readOnly", "setReadOnly"),
                               enumOf("textAlign", "setAlign", "TextAlign",
                                      {"left", "center", "right"}),
                           }});

  out.push_back(WidgetSpec{"ImageView", "View", false,
                           {
                               // image 属性这里刻意不提供：位图必须由平台加载后
                               // 直接调 setImage()。XML 里写文件路径会让「布局」
                               // 依赖运行期的文件系统，产物就不是单个 exe 了。
                               enumOf("icon", "setIcon", "IconId",
                                      {"none", "check", "chevronDown", "chevronRight",
                                       "chevronUp", "minimize", "maximize", "restore",
                                       "close", "search", "folder", "file"}),
                               enumOf("scale", "setScale", "ImageScale",
                                      {"fit", "fill", "stretch", "center"}),
                               color("iconColor", "setIconColor"),
                           }});

  out.push_back(WidgetSpec{"ProgressBar", "View", false,
                           {
                               num("progress", "setProgress", ValueType::kFloat),
                               flag("indeterminate", "setIndeterminate"),
                               flag("showPercent", "setShowPercent"),
                               text("label", "setLabel"),
                               design("thickness", "setTrackThickness"),
                           }});

  out.push_back(WidgetSpec{"Slider", "View", false,
                           {
                               num("min", "setMinimum", ValueType::kDouble),
                               num("max", "setMaximum", ValueType::kDouble),
                               num("value", "setValue", ValueType::kDouble),
                               num("step", "setStep", ValueType::kDouble),
                           }});

  return out;
}

}  // namespace

const std::vector<WidgetSpec>& all() {
  static const std::vector<WidgetSpec> kAll = buildAll();
  return kAll;
}

const std::vector<AttributeSpec>& commonAttributes() {
  static const std::vector<AttributeSpec> kOut = buildCommon();
  return kOut;
}

const std::vector<AttributeSpec>& containerAttributes() {
  static const std::vector<AttributeSpec> kOut = buildContainer();
  return kOut;
}

const std::vector<AttributeSpec>& layoutParamAttributes() {
  static const std::vector<AttributeSpec> kOut = buildLayoutParams();
  return kOut;
}

namespace {

const AttributeSpec* findIn(const std::vector<AttributeSpec>& list, const std::string& name) {
  for (const auto& spec : list) {
    if (spec.name == name) return &spec;
  }
  return nullptr;
}

const WidgetSpec* findRaw(const std::string& tag) {
  for (const auto& spec : all()) {
    if (spec.tag == tag) return &spec;
  }
  return nullptr;
}

}  // namespace

const WidgetSpec* find(const std::string& tag) {
  if (const WidgetSpec* spec = findRaw(tag)) return spec;
  // ViewGroup 只作为容器的基类存在，不是可直接写出的标签
  if (tag == "ViewGroup") {
    static const WidgetSpec kGroup{"ViewGroup", "View", true, {}};
    return &kGroup;
  }
  return nullptr;
}

const AttributeSpec* findAttribute(const std::string& tag, const std::string& attribute) {
  // 顺序即优先级：布局参数 > 共有属性 > 容器属性 > 控件自身 > 基类链
  if (const AttributeSpec* spec = findIn(layoutParamAttributes(), attribute)) return spec;
  if (const AttributeSpec* spec = findIn(commonAttributes(), attribute)) return spec;

  const WidgetSpec* spec = find(tag);
  if (spec == nullptr) return nullptr;

  if (spec->container) {
    if (const AttributeSpec* found = findIn(containerAttributes(), attribute)) return found;
  }
  if (const AttributeSpec* found = findIn(spec->attributes, attribute)) return found;

  // 沿基类链查：ViewGroup 的容器属性、View 的共有属性已在上面覆盖，
  // 这里主要处理「某容器继承另一个容器」的未来情况。
  std::string base = spec->base;
  while (!base.empty()) {
    const WidgetSpec* parent = find(base);
    if (parent == nullptr) break;
    if (parent->container) {
      if (const AttributeSpec* found = findIn(containerAttributes(), attribute)) return found;
    }
    if (const AttributeSpec* found = findIn(parent->attributes, attribute)) return found;
    base = parent->base;
  }
  return nullptr;
}

bool accepts(const std::string& tag, const std::string& attribute) {
  return findAttribute(tag, attribute) != nullptr;
}

std::string validateCombination(const std::string& tag,
                                const std::vector<std::pair<std::string, std::string>>& attrs) {
  auto valueOf = [&attrs](const std::string& key) -> const std::string* {
    for (const auto& kv : attrs) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  };

  if (tag == "Slider") {
    const std::string* minText = valueOf("min");
    const std::string* maxText = valueOf("max");
    if (minText != nullptr && maxText != nullptr) {
      const double lo = std::strtod(minText->c_str(), nullptr);
      const double hi = std::strtod(maxText->c_str(), nullptr);
      if (lo >= hi) return "min 必须小于 max";
    }
  }

  if (tag == "ProgressBar") {
    const std::string* indeterminate = valueOf("indeterminate");
    const std::string* progress = valueOf("progress");
    if (indeterminate != nullptr && *indeterminate == "true" && progress != nullptr) {
      return "indeterminate=\"true\" 与 progress 互斥：不确定态没有进度值";
    }
  }

  if (tag == "TextField") {
    const std::string* password = valueOf("password");
    const std::string* textValue = valueOf("text");
    if (password != nullptr && *password == "true" && textValue != nullptr) {
      return "密码框不要写死 text：明文初始值会出现在布局文件里";
    }
  }

  return {};
}

}  // namespace specs
}  // namespace pyui
