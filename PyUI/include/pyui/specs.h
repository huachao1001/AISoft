// 控件属性清单（spec）—— 只被构建期工具 pyui_layoutc 与测试使用
//
// 这张表是「XML 写错能在编译期报错」的唯一依据，同时也是生成器知道
// 「哪个属性调哪个 setter」的唯一依据。它不进发布产物：CMake 里
// pyui_specs 是独立静态库，只有工具链接它，应用不链接。
//
// 为什么必须有这张表：C++ 编译器只能报「没有 setTextSizePx 这个成员」，
// 报不出「XML 第 14 行写了 size，而 TextView 的属性叫 textSize」，也给不出
// 行列号。只有生成器知道行列号，所以校验必须发生在生成之前。
//
// 与实现同源靠 tests/test_specs.cpp 保证：逐个断言这里的 setter 在真实
// 控件上存在、签名兼容。加了 setter 忘加 spec，测试就红。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace pyui {
namespace specs {

// 属性值类型，决定生成器怎么把它转成 C++ 实参
enum class ValueType {
  kString,     // 字符串字面量
  kText,       // 文本内容（UTF-8，需转义）
  kInt,        // 整数
  kBool,       // true / false
  kFloat,      // 浮点（C++ float）
  kDouble,     // 浮点（C++ double）：累加误差敏感的数值用它
  kColor,      // "#RRGGBB" / "#AARRGGBB"
  kEnum,       // 枚举名，取值见 allowedValues，生成 EnumType::kName
  kDesign,     // 96 DPI 设计值，整数
  kId,         // 视图 id，生成 setId(makeId("..."))
  kDimension,  // width/height：match / wrap / 数字（设计值）
};

struct AttributeSpec {
  std::string name;
  ValueType type = ValueType::kString;

  // 生成代码里调用的 setter 名，如 "setText"。为空表示由 type 推导。
  std::string setter;
  // kEnum 的枚举类型名，如 "TextSize"；决定生成 TextSize::kLarge
  std::string enumType;
  // kEnum 的合法取值（小写，对应 kName 去掉 k 再小写）
  std::vector<std::string> allowedValues;

  // 是否布局参数（width/height/weight/margin/align），由容器消费而非控件
  bool layoutParam = false;
  bool required = false;
  // 一句话说明，报错时一起打出来
  std::string note;
};

struct WidgetSpec {
  // XML 标签名，须与 className() 一致
  std::string tag;
  // 基类标签名，用于属性继承（"View" / "ViewGroup"）
  std::string base;
  bool container = false;
  std::vector<AttributeSpec> attributes;
};

const std::vector<WidgetSpec>& all();
const WidgetSpec* find(const std::string& tag);

// 所有控件共有属性：id / visible / enabled / background
const std::vector<AttributeSpec>& commonAttributes();
// 所有容器共有属性：padding / gravity / spacing
const std::vector<AttributeSpec>& containerAttributes();
// 布局参数属性：width / height / weight / margin / align / x / y
const std::vector<AttributeSpec>& layoutParamAttributes();

// 属性组合是否合法。返回空串表示合法，否则是可读原因。
// 例：Slider 的 min >= max；ProgressBar 同时写了 indeterminate 与 progress。
std::string validateCombination(const std::string& tag,
                                const std::vector<std::pair<std::string, std::string>>& attrs);

// 查属性描述（沿继承链）。找不到返回 nullptr。
const AttributeSpec* findAttribute(const std::string& tag, const std::string& attribute);

// 这个标签（含继承链）是否允许该属性
bool accepts(const std::string& tag, const std::string& attribute);

}  // namespace specs
}  // namespace pyui
