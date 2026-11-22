// 属性表与控件实现的一致性测试
//
// 这张表是「XML 写错能在编译期报错」的唯一依据，所以它必须与真实控件
// 严格同步。同步分两半：
//
//   1. 表里写的 setter 必须真的存在，且参数类型能接受生成器给出的字面量
//      ——这一半靠 std::is_invocable 在**编译期**断言，漏写就直接编译失败；
//   2. 表与实现不能各说各话（表里有、控件没有；控件有、表里漏）——这一半
//      在运行期逐条比对属性名集合。
//
// 为什么不用「字符串查成员函数」这种反射式做法：C++ 没有反射，猜名字的
// 测试只能测到「字符串拼写」，测不到「签名兼容」。而签名不兼容正是这里
// 最容易犯的错（比如 setProgress 接受 float，表里却标成 kDouble）。
#include "test_framework.h"

#include "pyui/layouts/frame_layout.h"
#include "pyui/layouts/linear_layout.h"
#include "pyui/specs.h"
#include "pyui/widgets/button.h"
#include "pyui/widgets/check_box.h"
#include "pyui/widgets/progress_bar.h"
#include "pyui/widgets/radio_button.h"
#include "pyui/widgets/slider.h"
#include "pyui/widgets/text_field.h"
#include "pyui/widgets/text_view.h"

#include <type_traits>

namespace pyui {
namespace test {
namespace {

using specs::AttributeSpec;
using specs::ValueType;

// 「属性名 + 值类型 + 已做过签名检查」的条目。
// 签名检查本身在编译期由 CHK 宏里的 static_assert 完成，所以运行期只需要
// 名字与类型两列来比对属性表。
struct CheckedAttr {
  std::string name;
  ValueType type;
};

}  // namespace
}  // namespace test
}  // namespace pyui

//   CHK(Widget, "属性名", ValueType::kXxx, setter, ArgType)
// 展开成一条编译期签名断言 + 一条运行期条目。
// 直接写 &Widget::setter 而不是字符串：拼错成员名编译不过，签名不兼容也编译不过。
#define CHK(widget, attrName, attrType, setter, argType)                                  \
  do {                                                                                     \
    static_assert(std::is_invocable_v<decltype(&widget::setter), widget*, argType>,        \
                  #widget "::" #setter " 不能接受 " #argType " 实参");                     \
    entries.push_back(CheckedAttr{attrName, attrType});                                    \
  } while (false)

namespace pyui {
namespace test {
namespace {

std::vector<CheckedAttr> buttonAttrs() {
  std::vector<CheckedAttr> entries;
  CHK(Button, "text", ValueType::kText, setLabel, std::string);
  CHK(Button, "style", ValueType::kEnum, setStyle, ButtonStyle);
  CHK(Button, "icon", ValueType::kEnum, setIcon, IconId);
  CHK(Button, "iconPlacement", ValueType::kEnum, setIconPlacement, IconPlacement);
  CHK(Button, "iconSize", ValueType::kDesign, setIconSize, int);
  CHK(Button, "padX", ValueType::kDesign, setPadX, int);
  CHK(Button, "radius", ValueType::kDesign, setRadius, int);
  return entries;
}

std::vector<CheckedAttr> textViewAttrs() {
  std::vector<CheckedAttr> entries;
  CHK(TextView, "text", ValueType::kText, setText, std::string);
  CHK(TextView, "textSize", ValueType::kEnum, setTextSize, TextSize);
  CHK(TextView, "bold", ValueType::kBool, setBold, bool);
  CHK(TextView, "textAlign", ValueType::kEnum, setAlign, TextAlign);
  CHK(TextView, "ellipsis", ValueType::kBool, setEllipsis, bool);
  CHK(TextView, "middleEllipsis", ValueType::kBool, setMiddleEllipsis, bool);
  CHK(TextView, "color", ValueType::kColor, setColor, Color);
  return entries;
}

std::vector<CheckedAttr> checkBoxAttrs() {
  std::vector<CheckedAttr> entries;
  CHK(CheckBox, "text", ValueType::kText, setLabel, std::string);
  CHK(CheckBox, "checked", ValueType::kBool, setChecked, bool);
  CHK(CheckBox, "tristate", ValueType::kBool, setTristate, bool);
  return entries;
}

std::vector<CheckedAttr> radioAttrs() {
  std::vector<CheckedAttr> entries;
  CHK(RadioButton, "text", ValueType::kText, setLabel, std::string);
  CHK(RadioButton, "checked", ValueType::kBool, setChecked, bool);
  return entries;
}

std::vector<CheckedAttr> textFieldAttrs() {
  std::vector<CheckedAttr> entries;
  CHK(TextField, "text", ValueType::kText, setText, std::string);
  CHK(TextField, "placeholder", ValueType::kText, setPlaceholder, std::string);
  CHK(TextField, "password", ValueType::kBool, setPassword, bool);
  CHK(TextField, "readOnly", ValueType::kBool, setReadOnly, bool);
  CHK(TextField, "textAlign", ValueType::kEnum, setAlign, TextAlign);
  return entries;
}

std::vector<CheckedAttr> progressAttrs() {
  std::vector<CheckedAttr> entries;
  // progress 是 float：生成器写 0.35f，kDouble 会生成 0.35（double）也能隐式
  // 转 float，所以这里刻意只断言 float，提醒改动时注意精度语义。
  CHK(ProgressBar, "progress", ValueType::kFloat, setProgress, float);
  CHK(ProgressBar, "indeterminate", ValueType::kBool, setIndeterminate, bool);
  CHK(ProgressBar, "showPercent", ValueType::kBool, setShowPercent, bool);
  CHK(ProgressBar, "label", ValueType::kText, setLabel, std::string);
  CHK(ProgressBar, "thickness", ValueType::kDesign, setTrackThickness, int);
  return entries;
}

std::vector<CheckedAttr> sliderAttrs() {
  std::vector<CheckedAttr> entries;
  // Slider 一律 double：0..1 上用 float 走 100 步会累积误差，
  // 用户拖到最右却停在 99%。表里必须是 kDouble，否则生成的字面量
  // 会在 .0f 上丢精度。
  CHK(Slider, "min", ValueType::kDouble, setMinimum, double);
  CHK(Slider, "max", ValueType::kDouble, setMaximum, double);
  CHK(Slider, "value", ValueType::kDouble, setValue, double);
  CHK(Slider, "step", ValueType::kDouble, setStep, double);
  return entries;
}

std::vector<CheckedAttr> containerAttrs() {
  std::vector<CheckedAttr> entries;
  // padding 有多个重载，CHK 里 &Widget::setter 取不到唯一地址，所以这里
  // 显式写出要检查的那个重载。三态 padding 最终都落在 setPadding(int)。
  static_assert(std::is_invocable_v<decltype(static_cast<void (ViewGroup::*)(int)>(
                                        &ViewGroup::setPadding)),
                                    ViewGroup*, int>,
                "setPadding(int) 不存在，生成器的 padding 代码会编译失败");
  // paddingHorizontal/Vertical 没有独立 setter：生成器把它们合并成
  // setPadding(h, v) 两参重载，且强制成对出现。
  static_assert(std::is_invocable_v<decltype(static_cast<void (ViewGroup::*)(int, int)>(
                                        &ViewGroup::setPadding)),
                                    ViewGroup*, int, int>,
                "setPadding(int,int) 不存在，生成器的成对 padding 代码会编译失败");
  entries.push_back(CheckedAttr{"padding", ValueType::kDesign});
  entries.push_back(CheckedAttr{"paddingHorizontal", ValueType::kDesign});
  entries.push_back(CheckedAttr{"paddingVertical", ValueType::kDesign});
  return entries;
}

std::vector<CheckedAttr> linearLayoutAttrs() {
  std::vector<CheckedAttr> entries;
  CHK(LinearLayout, "orientation", ValueType::kEnum, setOrientation, Orientation);
  CHK(LinearLayout, "spacing", ValueType::kDesign, setSpacing, int);
  CHK(LinearLayout, "gravity", ValueType::kEnum, setGravity, Gravity);
  return entries;
}

std::vector<CheckedAttr> commonAttrs() {
  std::vector<CheckedAttr> entries;
  CHK(View, "visible", ValueType::kBool, setVisible, bool);
  CHK(View, "enabled", ValueType::kBool, setEnabled, bool);
  CHK(View, "background", ValueType::kColor, setBackgroundColor, Color);
  // 窗口角色：XML 用 role 声明标题栏/按钮身份，平台判定只看角色，
  // 不看 id 也不看坐标——这样按钮放哪由布局决定。
  CHK(View, "role", ValueType::kEnum, setWindowRole, WindowRole);
  // id 是特例：生成器走 makeId，不走 setId 的字符串重载
  static_assert(std::is_invocable_v<decltype(static_cast<void (View::*)(const char*)>(
                                        &View::setId)),
                                    View*, const char*>,
                "setId(const char*) 不存在，生成器的 id 代码会编译失败");
  entries.push_back(CheckedAttr{"id", ValueType::kId});
  return entries;
}

// ── 比对 ──

// 表与实现必须一一对应：少一条（控件有、表里漏 → XML 写不出来）
// 或多一条（表里有、控件没有 → 生成代码编不过）都要报。
void checkCoverage(const std::string& tag, const std::vector<specs::AttributeSpec>& declared,
                   const std::vector<CheckedAttr>& checked) {
  for (const auto& spec : declared) {
    bool found = false;
    for (const auto& item : checked) {
      if (spec.name != item.name) continue;
      found = true;
      CHECK_MSG(spec.type == item.type,
                tag + " 的属性 " + spec.name + " 在表里是另一种 ValueType");
      break;
    }
    CHECK_MSG(found, tag + " 的属性 " + spec.name + " 没有对应的 setter 检查项");
  }

  for (const auto& item : checked) {
    bool found = false;
    for (const auto& spec : declared) {
      if (spec.name == item.name) {
        found = true;
        break;
      }
    }
    CHECK_MSG(found, std::string("检查项 ") + item.name + " 在 " + tag + " 的属性表里不存在");
  }
}

const specs::WidgetSpec* requireSpec(const char* tag) {
  const specs::WidgetSpec* spec = specs::find(tag);
  CHECK_MSG(spec != nullptr, std::string("属性表里缺少控件 ") + tag);
  return spec;
}

// 枚举取值必须与真实枚举一一对应，且顺序一致（生成器按下标拼 kName）。
// 顺序错了不会编译失败，只会让 style="primary" 生成 ButtonStyle::kPrimary
// 之外的另一个值——那是最难查的一类错。
void checkEnumValues(const std::string& tag, const char* attrName,
                     std::initializer_list<const char*> expected, int enumCount) {
  const AttributeSpec* spec = specs::findAttribute(tag, attrName);
  if (spec == nullptr) {
    FAIL_AT(tag + " 缺少枚举属性 " + attrName);
    return;
  }
  CHECK_EQ(spec->allowedValues.size(), expected.size());
  CHECK_EQ(static_cast<int>(spec->allowedValues.size()), enumCount);
  int index = 0;
  for (const char* name : expected) {
    if (index >= static_cast<int>(spec->allowedValues.size())) break;
    CHECK_EQ(spec->allowedValues[static_cast<size_t>(index)], std::string(name));
    ++index;
  }
}

}  // namespace

TEST(specs_widgets_have_matching_setters) {
  if (const auto* spec = requireSpec("TextView")) checkCoverage("TextView", spec->attributes, textViewAttrs());
  if (const auto* spec = requireSpec("Button")) checkCoverage("Button", spec->attributes, buttonAttrs());
  if (const auto* spec = requireSpec("CheckBox")) checkCoverage("CheckBox", spec->attributes, checkBoxAttrs());
  if (const auto* spec = requireSpec("RadioButton")) checkCoverage("RadioButton", spec->attributes, radioAttrs());
  if (const auto* spec = requireSpec("TextField")) checkCoverage("TextField", spec->attributes, textFieldAttrs());
  if (const auto* spec = requireSpec("ProgressBar")) checkCoverage("ProgressBar", spec->attributes, progressAttrs());
  if (const auto* spec = requireSpec("Slider")) checkCoverage("Slider", spec->attributes, sliderAttrs());
  if (const auto* spec = requireSpec("LinearLayout")) checkCoverage("LinearLayout", spec->attributes, linearLayoutAttrs());
  if (const auto* spec = requireSpec("FrameLayout")) checkCoverage("FrameLayout", spec->attributes, {});

  checkCoverage("ViewGroup", specs::containerAttributes(), containerAttrs());
  checkCoverage("View", specs::commonAttributes(), commonAttrs());
}

TEST(specs_enum_values_match_enum_order) {
  // 这些名字与生成器的约定必须一致：allowedValues[i] → Enum::k<CamelCase>
  checkEnumValues("TextView", "textSize", {"small", "normal", "large", "huge"},
                  static_cast<int>(TextSize::kHuge) + 1);
  checkEnumValues("TextView", "textAlign", {"left", "center", "right"},
                  static_cast<int>(TextAlign::kRight) + 1);
  checkEnumValues("Button", "style", {"outline", "subtle", "flat", "ghost", "primary"},
                  static_cast<int>(ButtonStyle::kPrimary) + 1);
  checkEnumValues("Button", "iconPlacement", {"none", "left", "top", "only"},
                  static_cast<int>(IconPlacement::kOnly) + 1);
  checkEnumValues("Button", "icon",
                  {"none", "check", "chevronDown", "chevronRight", "chevronUp", "minimize",
                   "maximize", "restore", "close", "search", "folder", "file"},
                  static_cast<int>(IconId::kFile) + 1);
  checkEnumValues("LinearLayout", "orientation", {"horizontal", "vertical"},
                  static_cast<int>(Orientation::kVertical) + 1);
  checkEnumValues("LinearLayout", "gravity", {"start", "center", "end"},
                  static_cast<int>(Gravity::kEnd) + 1);
}

TEST(specs_attribute_priority_is_documented) {
  // 布局参数优先于控件自身属性。align 之所以从「文字对齐」改名成 textAlign，
  // 就是因为 align 被布局参数占据；这条测试锁住这个约定，防止有人为了
  // 「顺手」又把文字对齐改回 align。
  const AttributeSpec* align = specs::findAttribute("TextView", "align");
  CHECK(align != nullptr);
  if (align != nullptr) {
    CHECK_MSG(align->layoutParam, "align 必须是布局参数，文字对齐请用 textAlign");
  }

  const AttributeSpec* textAlign = specs::findAttribute("TextView", "textAlign");
  CHECK(textAlign != nullptr);
  if (textAlign != nullptr) {
    CHECK(!textAlign->layoutParam);
    CHECK_EQ(textAlign->setter, std::string("setAlign"));
  }

  // width/height 是布局参数，不能被同名的控件属性抢走
  const AttributeSpec* width = specs::findAttribute("Button", "width");
  CHECK(width != nullptr);
  if (width != nullptr) CHECK(width->layoutParam);
}

TEST(specs_container_attributes_are_container_only) {
  // gravity / spacing 是 LinearLayout 独占：放进公共容器表会让 FrameLayout
  // 也「接受」它们，生成器随即产出调不到 setter 的代码。
  CHECK(specs::findAttribute("FrameLayout", "gravity") == nullptr);
  CHECK(specs::findAttribute("FrameLayout", "spacing") == nullptr);
  CHECK(specs::findAttribute("LinearLayout", "gravity") != nullptr);
  CHECK(specs::findAttribute("LinearLayout", "spacing") != nullptr);

  // padding 反过来必须所有容器都有（含嵌套的 LinearLayout）
  CHECK(specs::accepts("FrameLayout", "padding"));
  CHECK(specs::accepts("LinearLayout", "padding"));
  CHECK(specs::accepts("LinearLayout", "paddingHorizontal"));
}

TEST(specs_unknown_names_are_rejected) {
  // 生成器靠「查不到 → 报错」来拦住笔误，所以这里必须返回空
  CHECK(specs::find("Unknown") == nullptr);
  CHECK(specs::findAttribute("Button", "wide") == nullptr);
  CHECK(specs::findAttribute("TextView", "textSizePx") == nullptr);
  CHECK(!specs::accepts("Button", "padding"));
}

TEST(specs_combination_rules) {
  using Attrs = std::vector<std::pair<std::string, std::string>>;

  // Slider 的 min 必须小于 max
  CHECK(!specs::validateCombination("Slider", Attrs{{"min", "9"}, {"max", "1"}}).empty());
  CHECK(specs::validateCombination("Slider", Attrs{{"min", "1"}, {"max", "9"}}).empty());

  // 不确定态没有进度值
  CHECK(!specs::validateCombination("ProgressBar",
                                    Attrs{{"indeterminate", "true"}, {"progress", "0.5"}})
             .empty());
  CHECK(specs::validateCombination("ProgressBar", Attrs{{"indeterminate", "true"}}).empty());

  // 密码框不许写死明文
  CHECK(!specs::validateCombination("TextField", Attrs{{"password", "true"}, {"text", "abc"}})
             .empty());
  CHECK(specs::validateCombination("TextField", Attrs{{"password", "true"}}).empty());
}

}  // namespace test
}  // namespace pyui
