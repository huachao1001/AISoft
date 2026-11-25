// 跑一份字节码，把全局变量按 JSON 打印到 stdout。
//
// ── 为什么需要这个工具 ──
//
// 语义对拍的基准是 CPython（见 tests/check_vm_semantics.py）。要让两边能比，
// AiPy 侧必须能把「跑完之后每个变量的值」原样交出来。把它做成独立的小
// 可执行文件而不是塞进测试里，是因为「跑一个脚本看结果」本身也是调试时
// 最常用的动作——`aipy_vm_dump app.bc` 就能看到所有状态。
#include "aipy/bytecode.h"
#include "aipy/vm.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::string jsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

// 只输出 Python 侧也对拍得到的类型。容器递归展开。
std::string toJson(const aipy::Value& value) {
  switch (value.type()) {
    case aipy::Value::Type::kNone:
      return "null";
    case aipy::Value::Type::kBool:
      return value.toBool() ? "true" : "false";
    case aipy::Value::Type::kInt:
      return std::to_string(value.toInt());
    case aipy::Value::Type::kFloat: {
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "%.17g", value.toFloat());
      return buffer;
    }
    case aipy::Value::Type::kString:
      return "\"" + jsonEscape(value.asString()) + "\"";
    case aipy::Value::Type::kList: {
      std::string out = "[";
      const aipy::Value::List& items = value.asList();
      for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ", ";
        out += toJson(items[i]);
      }
      return out + "]";
    }
    case aipy::Value::Type::kDict: {
      std::string out = "{";
      const aipy::Value::List& keys = value.dictKeys();
      const aipy::Value::List& values = value.dictValues();
      for (std::size_t i = 0; i < keys.size() && i < values.size(); ++i) {
        if (i > 0) out += ", ";
        out += "\"" + jsonEscape(keys[i].toString()) + "\": " + toJson(values[i]);
      }
      return out + "}";
    }
    case aipy::Value::Type::kHost:
      return "\"<host>\"";
  }
  return "null";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "用法: aipy_vm_dump <file.bc>\n");
    return 2;
  }

  // 这个工具的 stdout 必须是纯 JSON（tests/check_vm_semantics.py 要解析它）。
  // 所以脚本里的 `print` 保持默认走 stderr——想看程序输出用 aipy_run，
  // 「机器读状态」和「人看输出」是两个不同的用途，混在一条流里会互相破坏。

  std::ifstream file(argv[1], std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "读不到 %s\n", argv[1]);
    return 2;
  }
  const std::vector<char> raw{std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>()};
  const std::vector<std::uint8_t> data(raw.begin(), raw.end());

  aipy::bytecode::Result loaded = aipy::bytecode::load(data);
  if (loaded.failed) {
    std::fprintf(stderr, "加载失败: %s\n", loaded.error.c_str());
    return 1;
  }

  aipy::vm::Machine machine;
  const aipy::vm::Result result = machine.run(loaded.program);
  if (result.failed) {
    std::fprintf(stderr, "执行失败（第 %d 行）: %s\n", result.line, result.error.c_str());
    return 1;
  }

  const std::vector<aipy::Value>& globals = machine.globals();
  std::printf("{\"globals\": {");
  bool first = true;
  for (std::size_t i = 0; i < loaded.program.globalNames.size() && i < globals.size(); ++i) {
    // 以下划线开头的临时槽不导出：它们是编译器生成的
    // （\0assign_value 之类），CPython 里对应的东西根本不存在。
    const std::string& name = loaded.program.globalNames[i];
    if (name.empty() || name[0] == '\0' || name[0] == '_') continue;
    if (!first) std::printf(", ");
    first = false;
    std::printf("\"%s\": %s", jsonEscape(name).c_str(), toJson(globals[i]).c_str());
  }
  std::printf("}}\n");
  return 0;
}
