// pylib 注册表的实现：靠节合并把「已链接进来的扩展」串成两段连续内存。
//
// ── 机制 ──
//
// MSVC 把同名节（`.aipyreg$X`、`.aipyfn$X`）按 `$` 后的字母序合并。于是：
//
//     .aipyreg$a   哨兵             .aipyfn$a   哨兵
//     .aipyreg$k   各 pylib 的模块   .aipyfn$k   各导出函数
//     .aipyreg$z   哨兵             .aipyfn$z   哨兵
//
// 主程序扫描这两段区间，就能发现所有被链接进来的模块与函数。不需要初始化
// 函数、不需要静态构造顺序依赖、也不需要打包期生成任何 C++ 胶水代码。
//
// ── 为什么模块与函数分两节 ──
//
// 模块级解决「这个库参不参与」，函数级解决「库里哪个函数进产物」
// （Design.md 第 1.3 条要求精确到函数）。两者粒度不同，混在一节里会让
// 「只点名一个函数」和「整个模块进来」再也分不开。
//
// ── 为什么哨兵也要非 const ──
//
// 与 pylib.h 里那条注释同一个原因：`const` 对象是内部链接，`/O2` 会把
// 「没人引用的」直接消除。哨兵被消除了，扫描区间就变成垃圾地址。
#include "aipy/pylib.h"

#include <cstring>

#if defined(_MSC_VER)

extern "C" {
// 模块级哨兵。`$a` 保证排在最前，`$z` 排在最后。它们的值不重要，只用来取地址。
__declspec(allocate(".aipyreg$a")) ::aipy::pylib::Module aipy_pylib_begin = {
    nullptr};
__declspec(allocate(".aipyreg$z")) ::aipy::pylib::Module aipy_pylib_end = {
    nullptr};

// 函数级哨兵。同一个用途，只是换一节。
__declspec(allocate(".aipyfn$a")) ::aipy::pylib::Function aipy_pylib_fn_begin = {
    nullptr, nullptr, nullptr};
__declspec(allocate(".aipyfn$z")) ::aipy::pylib::Function aipy_pylib_fn_end = {
    nullptr, nullptr, nullptr};

// 裸名级哨兵。第三对，用途同上。
__declspec(allocate(".aipybare$a")) ::aipy::pylib::BuiltinName
    aipy_pylib_bare_begin = {nullptr, nullptr, nullptr};
__declspec(allocate(".aipybare$z")) ::aipy::pylib::BuiltinName
    aipy_pylib_bare_end = {nullptr, nullptr, nullptr};
}

namespace aipy {
namespace pylib {
namespace {

using Module = ::aipy::pylib::Module;
using Function = ::aipy::pylib::Function;
using BuiltinName = ::aipy::pylib::BuiltinName;

const Module* firstModule() { return &aipy_pylib_begin + 1; }
const Module* lastModule() { return &aipy_pylib_end; }
const Function* firstFunction() { return &aipy_pylib_fn_begin + 1; }
const Function* lastFunction() { return &aipy_pylib_fn_end; }
const BuiltinName* firstBare() { return &aipy_pylib_bare_begin + 1; }
const BuiltinName* lastBare() { return &aipy_pylib_bare_end; }

// 模块名是否真的注册过。
//
// 为什么要查这一步：`/INCLUDE` 可能只点了函数符号而没点模块符号，那种
// 产物里函数在、模块不在。放行它就等于「模块级注册可有可无」，而模块级
// 是「这个库参不参与本次链接」的唯一记录——查一下，缺了就报「没链接
// 进来」，比让脚本调到一个本不该存在的函数安全。
bool moduleRegistered(const char* name, std::size_t length) {
  for (const Module* it = firstModule(); it < lastModule(); ++it) {
    if (it->name == nullptr) continue;
    if (std::strlen(it->name) != length) continue;
    if (std::memcmp(it->name, name, length) == 0) return true;
  }
  return false;
}

}  // namespace

int linkedModuleCount() {
  int count = 0;
  for (const Module* it = firstModule(); it < lastModule(); ++it) {
    if (it->name != nullptr) ++count;
  }
  return count;
}

std::vector<const char*> linkedModules() {
  std::vector<const char*> names;
  for (const Module* it = firstModule(); it < lastModule(); ++it) {
    if (it->name != nullptr) names.push_back(it->name);
  }
  return names;
}

vm::ExtensionFn resolve(const char* qualified) {
  if (qualified == nullptr) return nullptr;
  // 按**最后一个**点切：函数名永远不含点，而模块名可以（`os.path.exists`）。
  //
  // 按第一个点切会把 `os.path.exists` 读成模块 `os` + 函数 `path.exists`，
  // 于是 moduleRegistered("os") 失败，运行时报「没有链接进来」——但产物里
  // 明明注册了 `os.path`。这个坑的症状是「链接信息正确、运行却说不存在」。
  const char* dot = std::strrchr(qualified, '.');
  if (dot == nullptr) return nullptr;
  const std::size_t moduleLength = static_cast<std::size_t>(dot - qualified);
  const char* function = dot + 1;
  if (*function == '\0') return nullptr;

  if (!moduleRegistered(qualified, moduleLength)) return nullptr;

  const std::size_t functionLength = std::strlen(function);
  for (const Function* it = firstFunction(); it < lastFunction(); ++it) {
    if (it->module == nullptr || it->name == nullptr || it->fn == nullptr) continue;
    // 先在**模块名**上比全等再比函数名：同名函数（比如两个库都有 `open`）
    // 靠这一层区分，前缀比较在这里是不够的。
    if (std::strlen(it->module) != moduleLength) continue;
    if (std::memcmp(it->module, qualified, moduleLength) != 0) continue;
    if (std::strlen(it->name) != functionLength) continue;
    if (std::memcmp(it->name, function, functionLength) != 0) continue;
    return it->fn;
  }
  return nullptr;
}

bool bareRegistered(const char* bare) {
  if (bare == nullptr) return false;
  const std::size_t length = std::strlen(bare);
  for (const BuiltinName* it = firstBare(); it < lastBare(); ++it) {
    if (it->bare == nullptr) continue;
    if (std::strlen(it->bare) != length) continue;
    if (std::memcmp(it->bare, bare, length) == 0) return true;
  }
  return false;
}

vm::ExtensionFn resolveBare(const char* bare) {
  if (bare == nullptr) return nullptr;
  const std::size_t length = std::strlen(bare);
  for (const BuiltinName* it = firstBare(); it < lastBare(); ++it) {
    if (it->bare == nullptr || it->module == nullptr ||
        it->name == nullptr) {
      continue;
    }
    if (std::strlen(it->bare) != length) continue;
    if (std::memcmp(it->bare, bare, length) != 0) continue;

    // 找到裸名了，但它所属的模块没被链接进来（只点了裸名项、没点模块项，
    // 或者库根本没链）。返回 nullptr，让调用方报「没有链接进来」——这与
    // 限定名的处理一致：注册项在、实现不在，就是没链接。
    const std::size_t moduleLength = std::strlen(it->module);
    if (!moduleRegistered(it->module, moduleLength)) return nullptr;

    // 再按模块+函数名找出实现。不能直接信裸名表里的信息：「函数是否真的
    // 被点进产物」由 .aipyfn 那节决定，裸名只说明「可以这么叫」。
    const std::size_t nameLength = std::strlen(it->name);
    for (const Function* fn = firstFunction(); fn < lastFunction(); ++fn) {
      if (fn->module == nullptr || fn->name == nullptr || fn->fn == nullptr) {
        continue;
      }
      if (std::strlen(fn->module) != moduleLength) continue;
      if (std::memcmp(fn->module, it->module, moduleLength) != 0) continue;
      if (std::strlen(fn->name) != nameLength) continue;
      if (std::memcmp(fn->name, it->name, nameLength) != 0) continue;
      return fn->fn;
    }
    return nullptr;
  }
  return nullptr;
}

std::vector<std::string> linkedBareNames() {
  std::vector<std::string> entries;
  for (const BuiltinName* it = firstBare(); it < lastBare(); ++it) {
    if (it->bare == nullptr || it->module == nullptr ||
        it->name == nullptr) {
      continue;
    }
    entries.push_back(std::string(it->bare) + " <- " + it->module + "." +
                      it->name);
  }
  return entries;
}

}  // namespace pylib
}  // namespace aipy

#else

// 非 MSVC 平台还没有分节合并的实现。这里保留**同一组函数**（返回空表而
// 不是编译失败）：pylib 的声明性接口是跨平台的，缺的只是「怎么把注册项
// 收进一节」这一步——那是各平台各自的实现细节（GNU ld 用
// `__attribute__((section(...)))` + `__start_/__stop_` 符号）。
//
// 但**用宏的库**在非 MSVC 上会编译失败而不是静默空表：静默空表意味着
// 「打包成功、运行时报扩展没链接」，那种症状比编译错误难查得多。
namespace aipy {
namespace pylib {
int linkedModuleCount() { return 0; }
std::vector<const char*> linkedModules() { return {}; }
vm::ExtensionFn resolve(const char*) { return nullptr; }
vm::ExtensionFn resolveBare(const char*) { return nullptr; }
bool bareRegistered(const char*) { return false; }
std::vector<std::string> linkedBareNames() { return {}; }
}  // namespace pylib
}  // namespace aipy

#endif
