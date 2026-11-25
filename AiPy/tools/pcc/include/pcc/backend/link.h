// 按需链接 pylib：把用到的 `.lib` 与 launcher 的 obj 重新链成 exe。
//
// ── 这一步在整条链路里的位置 ──
//
//     脚本 .py ──编译器──> .bc（含扩展表：只列真的调用的「模块.函数」）
//                              │
//                              ├─ 扩展表 → 链哪几个库、哪几个函数（本文件）
//                              │
//     launcher 的 obj + aipy.lib + 选中的 pylib ──link.exe──> 新 launcher
//
// **按需的落点就是这里**，而且是函数级：扩展表里没有 `threading.pending`，
// `aipy_threading_fn_pending` 就不出现在命令行，那个 obj 根本不被抽出。
//
// ── 为什么能按函数挑 ──
//
// 静态库的抽取粒度是 `.obj`。所以 pylib 侧让每个对外函数独立成一个编译
// 单元，打包时用 `/INCLUDE:<符号>` 逐个点名。
#pragma once

#include "pcc/backend/toolchain.h"

#include <string>
#include <vector>

namespace pcc {

// 一次按需链接的结果，给调用方打印用。
struct LinkResult {
  std::vector<std::string> libraries;       // 解出来的原生库路径
  std::vector<std::string> functions;       // 精确点名的函数（"模块.函数"）
  std::vector<std::string> symbols;         // 真实传给 /INCLUDE 的符号名
};

// 按模块名找 `<module>.pylib`。先看 libraryDir 本身，再看 `lib/` 子目录
// （dist 的布局）。找不到返回空串。
//
// 只认 `.pylib`：不提供裸 `.lib` 的查找。库的构建期形态与交付形态是同一个，
// 否则那条裸库路径永远不会被真实发布流程覆盖。
std::string findPylibArchive(const std::string& directory,
                             const std::string& module);

// 从扩展表里按模块名分组，保持首次出现顺序。
struct ModuleFunctions {
  std::string module;
  std::vector<std::string> functions;
};
// knownModules 用来判断点分模块名的边界：`os.path.exists` 里模块是
// `os.path` 而不是 `os`。不给（或匹配不上）时按第一个点切。
std::vector<ModuleFunctions> groupExtensions(
    const std::vector<std::string>& qualifiedNames,
    const std::vector<std::string>& knownModules = {});

// 重链接 launcher 到 output。成功返回 true。
//
// 两种模式，靠 `libraryDir` 里有没有 `lib/core.pylib` 区分：
//   dist  有 → 运行时从归档里解，buildDir 可以为空
//   构建  没有 → 运行时与入口都从 CMake 构建目录拿
//
// 链接器两种模式都由 toolchain 自己找（PATH → vswhere），不读任何配置文件。
//
// `scratchDir` 用来放从 `.pylib` 解出来的原生库：链接器只接受文件路径。
// 为空时退回 libraryDir（调用方应当总是给一个临时目录）。
bool relink(const std::string& buildDir, const std::string& libraryDir,
            const std::vector<std::string>& modules,
            const std::string& output,
            const std::vector<std::string>& extensions, LinkResult* result,
            std::string* error, const std::string& scratchDir,
            Subsystem subsystem = Subsystem::Console);

}  // namespace pcc
