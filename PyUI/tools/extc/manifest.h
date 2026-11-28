// 扩展清单的加载与校验
//
// 一份清单描述「一个扩展包提供什么」：模块名、Python 函数到 C++ 符号的映射、
// 每个平台对应的静态库、以及模块之间谁依赖谁。格式与 tools/pyuic/pylib.py 的
// manifest.json 是同一套——**schema 只能有一处实现**，否则「哪些 import 合法、
// 各自链哪个库」会散落到两边，迟早对不上。
//
// 与旧 extensions.py 的差别只有一点：那里的 "library" 是 CMake 目标名，
// 而用户机器上没有 CMake。这里只认 native.lib{平台: 包内路径}。
#pragma once

#include <map>
#include <string>
#include <vector>

namespace pyui {
namespace extc {

// 一个 Python 函数。arity 是 [最少, 最多]，与 pyuic 的 FREE_FUNCTIONS 一致。
struct Function {
  std::string python;  // 脚本里写的名字
  std::string cpp;     // 生成代码里调用的符号
  int minArgs = 0;
  int maxArgs = 0;
};

struct Module {
  std::string name;
  std::string doc;
  std::string bytecode;  // 包内路径，可为空
  std::vector<Function> functions;
  std::vector<std::string> includes;
  // 平台 → 包内相对路径。相对**清单文件所在目录**，这样包解到哪里都不用改。
  std::map<std::string, std::string> libs;
  // 直接依赖的模块名。用户只 import 顶层模块，闭包展开后才是要链的全部库。
  std::vector<std::string> depends;

  std::string manifestPath;  // 出处，报错时要指回去
  std::string packageDir;    // 清单所在目录，解析 libs 用

  const Function* findFunction(const std::string& pythonName) const;
};

// 全部已知扩展，按模块名索引。重名直接判错：两个包提供同一个模块名时，
// 链接进来的是哪一个完全取决于链接顺序，这种不确定性不该被静默接受。
struct Catalog {
  std::vector<Module> modules;

  const Module* find(const std::string& name) const;
  // 报错时列给用户看的可用模块清单
  std::string describeAvailable() const;
};

struct LoadResult {
  bool ok = false;
  std::string error;  // 空表示成功
  Catalog catalog;
};

// 读入并校验若干清单文件。任一文件出错都整体失败——半个清单拿去链接
// 只会得到「无法解析的外部符号」，那时已经看不出是清单的问题了。
LoadResult load(const std::vector<std::string>& manifestPaths);

// 依赖闭包：从用户直接使用的模块出发，展开 depends。
// 返回有序（先直接依赖、后传递依赖）的模块名；遇到未知模块返回错误。
bool resolveClosure(const Catalog& catalog, const std::vector<std::string>& used,
                    std::vector<std::string>* ordered, std::string* error);

// 拼路径并归一为 '/'。link.exe 两种分隔符都认，但混用会让清单既难读
// 也难 diff；包内路径是跨平台分发的，写死 '\' 在别的平台上就是错的。
std::string joinPath(const std::string& left, const std::string& right);

// 全文件归一为 '/'，用于把清单里的路径写进输出
std::string normalizeSlashes(std::string path);

}  // namespace extc
}  // namespace pyui
