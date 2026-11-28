// pyui_extc —— 扩展清单分析与链接清单生成
//
// 输入两样东西：
//   1. 若干 extension.json（扩展作者写的，声明「提供什么」）
//   2. used.json（pyuic 写的，声明「这个脚本用了什么」，带行号）
//
// 输出两样东西：
//   1. app.ext.json —— 人看的：用了哪些模块、各自链哪个库、体积归因
//   2. app.rsp      —— link.exe 吃的：把路径全部展开成绝对路径
//
// 为什么要有第二样：用户机器上只有 link.exe，没有 CMake也没有本工具时
// 也能重跑链接。响应文件是 link.exe 的原生输入格式，不需要任何中间层。
#include "manifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using pyui::extc::Catalog;
using pyui::extc::Module;

struct Options {
  std::string outJson;
  std::string outRsp;
  std::string platform = "win-x64";
  std::string baseDir;  // 输出里路径的前缀；空表示原样相对
  std::vector<std::string> manifests;
  std::string usedPath;
};

void printUsage() {
  std::printf(
      "用法: pyui_extc --used <used.json> --manifest <清单>... --out-json <路径> --out-rsp <路径>\n"
      "                [--platform <平台>] [--base-dir <目录>]\n"
      "\n"
      "  --used       pyuic 产出的「用到了哪些模块」清单\n"
      "  --manifest   扩展包的清单文件，可以给多个\n"
      "  --out-json   人看的链接清单（JSON）\n"
      "  --out-rsp    link.exe 的响应文件\n"
      "  --platform   目标平台，默认 win-x64\n"
      "  --base-dir   写进 .rsp 的路径前缀（用绝对路径，避免依赖当前目录）\n");
}

// 读 used.json：{"modules": [{"name": "fs", "functions": ["read_file"],
//                             "sites": [{"file": "...", "line": 3, "column": 1}]}]}
struct UsedModule {
  std::string name;
  std::vector<std::string> functions;
};

struct UsedFile {
  bool ok = false;
  std::string error;
  std::vector<UsedModule> modules;
};

UsedFile readUsed(const std::string& path) {
  UsedFile result;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    result.error = path + ": 打不开";
    return result;
  }

  nlohmann::json json;
  try {
    stream >> json;
  } catch (const std::exception& exception) {
    result.error = path + ": 不是合法 JSON：" + exception.what();
    return result;
  }

  const auto modules = json.find("modules");
  if (modules == json.end() || !modules->is_array()) {
    result.error = path + ": 缺少 \"modules\" 数组（应由 pyuic 生成）";
    return result;
  }

  for (const nlohmann::json& entry : *modules) {
    if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string()) {
      result.error = path + ": modules 里的每一项都必须是带 \"name\" 的对象";
      return result;
    }
    UsedModule module;
    module.name = entry["name"].get<std::string>();
    if (const auto functions = entry.find("functions"); functions != entry.end()) {
      if (!functions->is_array()) {
        result.error = path + ": 模块 \"" + module.name + "\" 的 functions 必须是数组";
        return result;
      }
      for (const nlohmann::json& item : *functions) {
        if (!item.is_string()) {
          result.error = path + ": 模块 \"" + module.name + "\" 的 functions 必须是字符串数组";
          return result;
        }
        module.functions.push_back(item.get<std::string>());
      }
    }
    result.modules.push_back(std::move(module));
  }
  result.ok = true;
  return result;
}

bool writeIfChanged(const std::string& path, const std::string& content, std::string* error) {
  {
    std::ifstream existing(path, std::ios::binary);
    if (existing) {
      std::string current((std::istreambuf_iterator<char>(existing)),
                          std::istreambuf_iterator<char>());
      if (current == content) return true;
    }
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    *error = path + ": 写不进去";
    return false;
  }
  stream << content;
  if (!stream) {
    *error = path + ": 写入失败";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "pyui_extc: %s 后面缺少参数\n", name);
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--help" || arg == "-h") {
      printUsage();
      return 0;
    } else if (arg == "--used") {
      const char* value = next("--used");
      if (value == nullptr) return 2;
      options.usedPath = value;
    } else if (arg == "--manifest") {
      const char* value = next("--manifest");
      if (value == nullptr) return 2;
      options.manifests.push_back(value);
    } else if (arg == "--out-json") {
      const char* value = next("--out-json");
      if (value == nullptr) return 2;
      options.outJson = value;
    } else if (arg == "--out-rsp") {
      const char* value = next("--out-rsp");
      if (value == nullptr) return 2;
      options.outRsp = value;
    } else if (arg == "--platform") {
      const char* value = next("--platform");
      if (value == nullptr) return 2;
      options.platform = value;
    } else if (arg == "--base-dir") {
      const char* value = next("--base-dir");
      if (value == nullptr) return 2;
      options.baseDir = value;
    } else {
      std::fprintf(stderr, "pyui_extc: 不认识的参数 %s\n", arg.c_str());
      printUsage();
      return 2;
    }
  }

  if (options.usedPath.empty() || options.outJson.empty() || options.outRsp.empty()) {
    std::fprintf(stderr, "pyui_extc: 必须给 --used、--out-json、--out-rsp\n");
    printUsage();
    return 2;
  }

  const UsedFile used = readUsed(options.usedPath);
  if (!used.ok) {
    std::fprintf(stderr, "pyui_extc: %s\n", used.error.c_str());
    return 1;
  }

  // 没有任何扩展时也要正常产出空清单：CMake 的 OUTPUT 列表必须稳定，
  // 否则每次构建都要重新 configure（与 extensions.py 的取舍一致）。
  const pyui::extc::LoadResult loaded = pyui::extc::load(options.manifests);
  if (!loaded.ok) {
    std::fprintf(stderr, "pyui_extc: %s\n", loaded.error.c_str());
    return 1;
  }
  const Catalog& catalog = loaded.catalog;

  std::vector<std::string> usedNames;
  usedNames.reserve(used.modules.size());
  for (const UsedModule& module : used.modules) usedNames.push_back(module.name);

  std::vector<std::string> ordered;
  std::string closureError;
  if (!pyui::extc::resolveClosure(catalog, usedNames, &ordered, &closureError)) {
    std::fprintf(stderr, "pyui_extc: %s\n%s", closureError.c_str(),
                 used.modules.empty() ? "\n（脚本没有 import 任何扩展）" : "");
    return 1;
  }

  // ── 输出 JSON ──
  nlohmann::json out;
  out["platform"] = options.platform;
  out["modules"] = nlohmann::json::array();
  std::vector<std::string> libraries;

  for (const std::string& name : ordered) {
    const Module* module = catalog.find(name);
    if (module == nullptr) continue;  // 闭包已经校验过，这里只是防御

    nlohmann::json entry;
    entry["name"] = module->name;
    // 是直接 import 的，还是被 depends 带进来的 —— 体积归因时这个区别很重要
    entry["direct"] = std::find(usedNames.begin(), usedNames.end(), name) != usedNames.end();
    if (!module->doc.empty()) entry["doc"] = module->doc;

    if (!module->functions.empty()) {
      nlohmann::json functions = nlohmann::json::array();
      for (const auto& function : module->functions) functions.push_back(function.python);
      entry["functions"] = functions;
    }

    if (!module->libs.empty()) {
      const auto it = module->libs.find(options.platform);
      if (it == module->libs.end()) {
        std::fprintf(stderr,
                     "pyui_extc: 模块 \"%s\" 没有 %s 的静态库（清单里有：%s）\n",
                     module->name.c_str(), options.platform.c_str(),
                     [&] {
                       std::string joined;
                       for (const auto& kv : module->libs) {
                         if (!joined.empty()) joined += "、";
                         joined += kv.first;
                       }
                       return joined;
                     }()
                         .c_str());
        return 1;
      }
      entry["lib"] = it->second;
      // 库路径相对清单所在目录：包解到哪都能用，清单不用改
      libraries.push_back(pyui::extc::joinPath(
          options.baseDir, pyui::extc::joinPath(module->packageDir, it->second)));
    }
    if (!module->depends.empty()) entry["depends"] = module->depends;

    out["modules"].push_back(std::move(entry));
  }

  out["libraries"] = libraries;

  const std::string jsonText = out.dump(2) + "\n";
  std::string writeError;
  if (!writeIfChanged(options.outJson, jsonText, &writeError)) {
    std::fprintf(stderr, "pyui_extc: %s\n", writeError.c_str());
    return 1;
  }

  // ── 输出响应文件 ──
  //
  // link.exe 的响应文件一条参数一行。路径里有空格时用引号括起来；
  // 我们不做进一步的转义，因为 MSVC 的引号规则比 POSIX 更宽松，
  // 而清单里的路径都是我们自己拼的。
  std::string rsp;
  for (const std::string& library : libraries) {
    rsp += '"';
    rsp += library;
    rsp += "\"\n";
  }
  if (!writeIfChanged(options.outRsp, rsp, &writeError)) {
    std::fprintf(stderr, "pyui_extc: %s\n", writeError.c_str());
    return 1;
  }

  std::printf("pyui_extc: %d 个模块 -> %s（%d 个静态库）\n",
              static_cast<int>(ordered.size()), options.outJson.c_str(),
              static_cast<int>(libraries.size()));
  return 0;
}
