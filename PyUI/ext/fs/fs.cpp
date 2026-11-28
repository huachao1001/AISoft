// fs 扩展的实现
//
// 与核心 py_script.cpp 里那四个同名函数逻辑一致——它们是同一段代码的迁移
// 目标，不是另写一份。等示例全部改成 import fs 之后，核心里那四个就会被删掉，
// 「核心不含任何 IO」这条才真正成立。
#include "pyui/ext/fs.h"

#include "pyui/platform/platform.h"

#include <cstdio>
#include <string>

namespace pyui {
namespace ext {
namespace fs {

Value readFile(const Value& path) {
  const std::string name = path.toString();
  if (name.empty()) return Value();

  std::FILE* file = std::fopen(name.c_str(), "rb");
  if (file == nullptr) return Value();

  std::string content;
  char buffer[4096];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) content.append(buffer, got);
  std::fclose(file);

  // 去掉 UTF-8 BOM：记事本保存的文本会带它，留在正文里第一个字是乱码。
  if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
      static_cast<unsigned char>(content[1]) == 0xBB &&
      static_cast<unsigned char>(content[2]) == 0xBF) {
    content.erase(0, 3);
  }
  return Value(std::move(content));
}

Value writeFile(const Value& path, const Value& text) {
  const std::string name = path.toString();
  if (name.empty()) return Value("路径为空");

  std::FILE* file = std::fopen(name.c_str(), "wb");
  if (file == nullptr) return Value("无法写入 " + name);

  const std::string content = text.toString();
  const size_t written = std::fwrite(content.data(), 1, content.size(), file);
  std::fclose(file);
  // 短写说明磁盘满了或配额用尽。必须报出来：静默截断一个笔记文件，
  // 用户会以为保存成功。
  if (written != content.size()) {
    return Value("只写出 " + std::to_string(written) + " / " +
                 std::to_string(content.size()) + " 字节");
  }
  return Value();
}

Value joinPath(const Value& left, const Value& right) {
  std::string head = left.toString();
  const std::string tail = right.toString();
  if (head.empty()) return Value(tail);
  if (tail.empty()) return Value(head);
  // 分隔符跟随平台：脚本里拼路径必须走这里，否则 "\" 在另一个平台
  // 会被当成文件名的一部分。
  const char sep = platform().pathSeparator();
  if (head.back() != sep && head.back() != '/' && head.back() != '\\') head.push_back(sep);
  return Value(std::move(head) + tail);
}

Value homeDirectory() {
  const std::string home = platform().homeDirectory();
  if (home.empty()) return Value();
  return Value(home);
}

}  // namespace fs
}  // namespace ext
}  // namespace pyui
