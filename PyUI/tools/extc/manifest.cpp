// 扩展清单的加载与校验（实现）
//
// 用 nlohmann/json 解析：清单是手写文件，报错信息必须指向「哪个字段」，
// 自己写的解析器在这件事上只会多一份要维护的代码。库只被工具链链接，
// 不进发布产物。
#include "manifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <set>

namespace pyui {
namespace extc {
namespace {

constexpr int kFormatVersion = 1;
const std::vector<std::string> kKnownPlatforms = {"win-x64", "win-arm64", "mac-arm64",
                                                  "linux-x64"};

// 包内静态库用各平台原生后缀。包本身是 .pylib（zip），解包得到的是真实文件，
// 没有「消费方要靠扩展名猜编译器」的问题，所以库文件不必再伪装。
std::string libSuffixFor(const std::string& platform) {
  if (platform == "win-x64" || platform == "win-arm64") return ".lib";
  return ".a";
}

std::string directoryOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// 路径归一：全部用 '/'。link.exe 两种都认，但混用会让清单文件既难读
// 也难 diff；而且包内路径是跨平台分发的，写死 '\' 在别的平台上就是错的。
std::string normalizeSlashesImpl(std::string path) {
  for (char& ch : path) {
    if (ch == '\\') ch = '/';
  }
  return path;
}

}  // namespace

std::string normalizeSlashes(std::string path) { return normalizeSlashesImpl(std::move(path)); }

// 去掉重复的分隔符，并处理「.」段。不做完整规范化（不解析 ..）：
// 清单里的路径是我们自己拼的，多余的 .. 只会掩盖清单写错。
std::string joinPath(const std::string& left, const std::string& right) {
  if (left.empty()) return normalizeSlashesImpl(right);
  if (right.empty()) return normalizeSlashesImpl(left);
  std::string base = normalizeSlashesImpl(left);
  std::string tail = normalizeSlashesImpl(right);
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  while (!tail.empty() && tail.front() == '/') tail.erase(tail.begin());
  return base + "/" + tail;
}

namespace {

std::string joinKnown(const std::vector<std::string>& items, const char* separator) {
  std::string joined;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) joined += separator;
    joined += items[i];
  }
  return joined;
}

// 取一个必填字符串字段。缺了或类型不对都当场报错并指出是哪个对象的哪个字段。
bool requireString(const nlohmann::json& object, const char* key, const std::string& where,
                   std::string* out, std::string* error) {
  const auto it = object.find(key);
  if (it == object.end()) {
    *error = where + ": 缺少字段 \"" + key + "\"";
    return false;
  }
  if (!it->is_string() || it->get<std::string>().empty()) {
    *error = where + ": \"" + key + "\" 必须是非空字符串";
    return false;
  }
  *out = it->get<std::string>();
  return true;
}

bool optionalString(const nlohmann::json& object, const char* key, const std::string& where,
                    std::string* out, std::string* error) {
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) return true;
  if (!it->is_string()) {
    *error = where + ": \"" + key + "\" 必须是字符串";
    return false;
  }
  *out = it->get<std::string>();
  return true;
}

// arity 必须是 [最少, 最多]，两个非负整数且前者不大于后者。
bool readArity(const nlohmann::json& entry, const std::string& where, int* minArgs,
               int* maxArgs, std::string* error) {
  const auto it = entry.find("arity");
  if (it == entry.end()) {
    *error = where + ": 缺少 \"arity\"（如 [1, 1]）";
    return false;
  }
  if (!it->is_array() || it->size() != 2 || !(*it)[0].is_number_integer() ||
      !(*it)[1].is_number_integer()) {
    *error = where + ": \"arity\" 必须是 [最少, 最多] 两个整数";
    return false;
  }
  *minArgs = (*it)[0].get<int>();
  *maxArgs = (*it)[1].get<int>();
  if (*minArgs < 0 || *maxArgs < *minArgs) {
    *error = where + ": \"arity\" 必须满足 0 <= 最少 <= 最多";
    return false;
  }
  return true;
}

bool loadModule(const nlohmann::json& json, const std::string& manifestPath, Module* out,
                std::string* error) {
  const std::string where = manifestPath + ": 模块";

  if (!requireString(json, "name", where, &out->name, error)) return false;
  const std::string moduleWhere = manifestPath + ": 模块 \"" + out->name + "\"";

  if (!optionalString(json, "doc", moduleWhere, &out->doc, error)) return false;
  if (!optionalString(json, "bytecode", moduleWhere, &out->bytecode, error)) return false;

  // ── functions ──
  if (const auto it = json.find("functions"); it != json.end()) {
    if (!it->is_array()) {
      *error = moduleWhere + ": \"functions\" 必须是数组";
      return false;
    }
    std::set<std::string> seen;
    for (size_t i = 0; i < it->size(); ++i) {
      const nlohmann::json& entry = (*it)[i];
      const std::string entryWhere =
          moduleWhere + " functions[" + std::to_string(i) + "]";
      if (!entry.is_object()) {
        *error = entryWhere + ": 必须是对象";
        return false;
      }
      Function function;
      if (!requireString(entry, "python", entryWhere, &function.python, error)) return false;
      if (!requireString(entry, "cpp", entryWhere, &function.cpp, error)) return false;
      if (!readArity(entry, entryWhere, &function.minArgs, &function.maxArgs, error)) {
        return false;
      }
      if (!seen.insert(function.python).second) {
        *error = entryWhere + ": 函数 \"" + function.python + "\" 重复声明";
        return false;
      }
      out->functions.push_back(std::move(function));
    }
  }

  // ── native ──
  if (const auto it = json.find("native"); it != json.end()) {
    if (!it->is_object()) {
      *error = moduleWhere + ": \"native\" 必须是对象";
      return false;
    }
    if (const auto includes = it->find("include"); includes != it->end()) {
      if (!includes->is_array()) {
        *error = moduleWhere + ".native: \"include\" 必须是路径数组";
        return false;
      }
      for (const nlohmann::json& item : *includes) {
        if (!item.is_string()) {
          *error = moduleWhere + ".native: \"include\" 必须是路径数组";
          return false;
        }
        out->includes.push_back(item.get<std::string>());
      }
    }
    if (const auto libs = it->find("lib"); libs != it->end()) {
      if (!libs->is_object()) {
        *error = moduleWhere + ".native: \"lib\" 必须是 {平台: 路径}";
        return false;
      }
      for (auto entry = libs->begin(); entry != libs->end(); ++entry) {
        const std::string platform = entry.key();
        if (std::find(kKnownPlatforms.begin(), kKnownPlatforms.end(), platform) ==
            kKnownPlatforms.end()) {
          *error = moduleWhere + ".native.lib: 未知平台 \"" + platform + "\"（可用：" +
                   joinKnown(kKnownPlatforms, "、") + "）";
          return false;
        }
        if (!entry.value().is_string() || entry.value().get<std::string>().empty()) {
          *error = moduleWhere + ".native.lib.\"" + platform + "\": 必须是包内路径";
          return false;
        }
        const std::string path = entry.value().get<std::string>();
        // 包内库用各平台原生后缀：Windows 是 .lib，macOS/Linux 是 .a。
        const std::string expected = libSuffixFor(platform);
        if (path.size() < expected.size() ||
            path.compare(path.size() - expected.size(), expected.size(), expected) != 0) {
          *error = moduleWhere + ".native.lib.\"" + platform + "\": 静态库必须以 " +
                   expected + " 结尾（当前是 \"" + path + "\"）";
          return false;
        }
        out->libs[platform] = path;
      }
    }
  }

  // ── depends ──
  if (const auto it = json.find("depends"); it != json.end()) {
    if (!it->is_array()) {
      *error = moduleWhere + ": \"depends\" 必须是模块名数组";
      return false;
    }
    for (const nlohmann::json& item : *it) {
      if (!item.is_string() || item.get<std::string>().empty()) {
        *error = moduleWhere + ": \"depends\" 必须是模块名数组";
        return false;
      }
      out->depends.push_back(item.get<std::string>());
    }
  }

  // 空模块解出来什么都不提供，几乎总是清单写漏了
  if (out->bytecode.empty() && out->functions.empty() && out->libs.empty()) {
    *error = moduleWhere + ": 既没有字节码、也没有函数或静态库，这样的包解出来是空的";
    return false;
  }

  out->manifestPath = manifestPath;
  out->packageDir = directoryOf(manifestPath);
  return true;
}

bool loadFile(const std::string& path, std::vector<Module>* out, std::string* error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    *error = path + ": 打不开";
    return false;
  }

  nlohmann::json json;
  try {
    stream >> json;
  } catch (const std::exception& exception) {
    *error = path + ": 不是合法 JSON：" + exception.what();
    return false;
  }

  if (!json.is_object()) {
    *error = path + ": 顶层必须是对象";
    return false;
  }

  const auto version = json.find("format");
  if (version == json.end() || !version->is_number_integer() ||
      version->get<int>() != kFormatVersion) {
    *error = path + ": 不支持的 format（本工具只认 " + std::to_string(kFormatVersion) +
             "）。包版本与新工具不匹配时，请用对应版本的工具打包";
    return false;
  }

  const auto modules = json.find("modules");
  if (modules == json.end() || !modules->is_array() || modules->empty()) {
    *error = path + ": \"modules\" 必须是非空数组";
    return false;
  }

  for (size_t i = 0; i < modules->size(); ++i) {
    Module module;
    if (!loadModule((*modules)[i], path, &module, error)) return false;
    out->push_back(std::move(module));
  }
  return true;
}

}  // namespace

const Function* Module::findFunction(const std::string& pythonName) const {
  for (const Function& function : functions) {
    if (function.python == pythonName) return &function;
  }
  return nullptr;
}

const Module* Catalog::find(const std::string& name) const {
  for (const Module& module : modules) {
    if (module.name == name) return &module;
  }
  return nullptr;
}

std::string Catalog::describeAvailable() const {
  if (modules.empty()) {
    return "（当前没有任何扩展。加一个扩展就是在 ext/ 下放一个 extension.json）";
  }
  std::string lines;
  for (const Module& module : modules) {
    std::string functions;
    for (size_t i = 0; i < module.functions.size(); ++i) {
      if (i > 0) functions += "、";
      functions += module.functions[i].python;
    }
    lines += "  " + module.name;
    if (!module.doc.empty()) lines += "（" + module.doc + "）";
    if (!functions.empty()) lines += "：" + functions;
    lines += "\n";
  }
  return lines;
}

LoadResult load(const std::vector<std::string>& manifestPaths) {
  LoadResult result;
  for (const std::string& path : manifestPaths) {
    std::vector<Module> modules;
    if (!loadFile(path, &modules, &result.error)) return result;

    for (Module& module : modules) {
      // 重名判错：最终链进来的是哪个库会取决于链接顺序，
      // 这种不确定性不该被静默接受。
      if (const Module* existing = result.catalog.find(module.name)) {
        result.error = "模块名 \"" + module.name + "\" 被两个清单重复定义：\n  " +
                       existing->manifestPath + "\n  " + module.manifestPath;
        return result;
      }
      result.catalog.modules.push_back(std::move(module));
    }
  }
  result.ok = true;
  return result;
}

bool resolveClosure(const Catalog& catalog, const std::vector<std::string>& used,
                    std::vector<std::string>* ordered, std::string* error) {
  ordered->clear();
  std::set<std::string> visited;
  // 队列里记「谁要求的谁」：报错时说「X 依赖的 Y 不存在」，
  // 否则用户只会看到缺失的模块名，不知道该改哪个清单。
  struct Pending {
    std::string name;
    std::string requiredBy;  // 空表示用户直接 import 的
  };
  std::vector<Pending> pending;
  for (const std::string& name : used) pending.push_back({name, std::string()});

  // 广度优先：直接依赖排在传递依赖前面。顺序本身不影响链接结果，
  // 但它决定了清单文件给人的阅读顺序——「我用到的」应该在最前面。
  for (size_t i = 0; i < pending.size(); ++i) {
    const std::string name = pending[i].name;
    if (!visited.insert(name).second) continue;

    const Module* module = catalog.find(name);
    if (module == nullptr) {
      if (pending[i].requiredBy.empty()) {
        *error = "没有名为 \"" + name + "\" 的扩展模块。当前可用：\n" +
                 catalog.describeAvailable();
      } else {
        *error = "模块 \"" + pending[i].requiredBy + "\" 依赖的 \"" + name +
                 "\" 不存在。请检查清单里的 depends";
      }
      return false;
    }
    ordered->push_back(name);
    for (const std::string& dependency : module->depends) {
      pending.push_back({dependency, module->name});
    }
  }
  return true;
}

}  // namespace extc
}  // namespace pyui
