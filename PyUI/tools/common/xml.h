// 极简 XML 解析器 —— 只给构建期工具用
//
// 为什么自己写而不引第三方：
//   布局 XML 只用到元素、属性、注释、文本极少，一个 500 行的解析器就够，
//   还不用为工具链引入 expat/pugixml 这类依赖。而且它不进发布产物——只被
//   pyui_layoutc 链接，所以这里可以用「够用就好」的标准。
//
// 刻意不支持：DTD、实体（除内置五个）、命名空间、CDATA 里的嵌套。
// 遇到不支持的东西就报错并给行列号，而不是静默猜——生成器最怕的就是
// 「猜错了却不报」。
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pyui {
namespace xml {

// 解析错误：带行列号，指向出错的那个字节
struct Error {
  int line = 0;
  int column = 0;
  std::string message;
  bool ok() const { return message.empty(); }
};

struct Node {
  std::string tag;
  // 属性按出现顺序保留：生成 setter 调用的顺序应与 XML 阅读顺序一致，
  // 否则两处看似无关的属性（如 setRange 与 setValue）会因为顺序不同
  // 而产生不同结果。
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<Node> children;
  // 元素前的文本内容；布局里基本不用，留作完整性
  std::string text;
  int line = 0;
  int column = 0;

  const std::string* attribute(const std::string& name) const {
    for (const auto& kv : attributes) {
      if (kv.first == name) return &kv.second;
    }
    return nullptr;
  }
};

struct Document {
  Node root;
  // 解析失败时为 false，error 里有原因
  bool ok = false;
  Error error;
};

// 从 UTF-8 文本解析。失败时 document.ok == false。
Document parse(std::string_view text);

// 解析文件；读不到时 ok == false 且 error.message 说明原因
Document parseFile(const std::string& path);

}  // namespace xml
}  // namespace pyui
