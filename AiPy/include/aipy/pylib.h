// pylib 的注册契约：让「已编译好的静态库」在链接进产物后能被 VM 发现。
//
// ── 为什么需要这一层 ──
//
// 设计理念第二条：C++ 编好的静态库（内置的、第三方的）以 pylib 的形式
// 与 VM 一起链接成 exe，且**按需**——脚本没 import 的库不进产物。
//
// 静态库有个天然麻烦：链接器只把「被引用到的」obj 从 .lib 里抽出来。
// 一个注册项如果没人引用，就被丢弃，主程序永远发现不了它。这个文件提供
// 的节合并（section merging）机制绕开了这件事：所有 pylib 把自己的
// 注册项写进同一个特殊节，主程序在运行时扫描这段连续内存。
//
// ── 它是语言机制，不是具体能力 ──
//
// 这里只有「怎么发现扩展」和「怎么按名字解析函数指针」，没有任何
// `text`/`fs`/`ui` 之类的东西。具体能力全在 pylib 里，符合「核心 VM
// 只认 Python 语法」。所以它能进 `aipy.lib` 而不破坏零界面依赖。
//
// ── 对标 pybind11：所有库用同一套宏 ──
//
// 系统库（`libs/threading`）和用户自定义库必须长得一样，否则「扩展能力」
// 就变成了两个标准。这里的宏就是全部：没有各库私有的 registry.h，
// 没有各库私有的符号前缀。
//
//   pybind11                        AiPy
//   ────────────────────────────    ─────────────────────────────────
//   PYBIND11_MODULE(text, m)   →    AIPY_PYLIB_MODULE(text);
//   m.def("upper", &upper)     →    AIPY_PYLIB_FUNCTION(text, upper, upper);
//
// **差别只有一处：AiPy 的函数注册必须在文件作用域单独一行。**
// pybind11 的 `m.def()` 可以写在 `PYBIND11_MODULE` 的块里，因为整个模块
// 编成一个 .so；AiPy 要求函数级按需链接（Design.md 第 1.3 条），而静态库
// 的抽取粒度是 .obj——几行 `m.def()` 写在同一个 .obj 里，`/INCLUDE` 点到
// 其中一个就会把同一个 obj 里其余实现一起拉进产物，按需就失效了。
// 所以每个函数一行、各自单独一个编译单元，宏写在文件作用域。
//
// ── 脚本侧看到的形态 ──
//
//   import text
//   text.upper("hi")      →  resolve("text.upper") → 被点名的那个 .obj
#pragma once

#include "aipy/vm.h"

#include <cstddef>
#include <string>
#include <vector>

// 节名布局：链接器把同名节按 `$` 后的字母序合并，所以扫描区间必然被
// 两个哨兵夹住。`$a` 在最前、`$z` 在最后是 MSVC 的约定。
//
// 三对节，各管一层：
//   .aipyreg   模块级（AIPY_PYLIB_MODULE）——「这个模块参不参与本次链接」
//   .aipyfn    函数级（AIPY_PYLIB_FUNCTION）——「这个模块的哪个函数进产物」
//   .aipybare  裸名级（AIPY_PYLIB_BUILTIN）——「这个模块给谁起了短名」
//
// 这段 `#pragma section` 必须出现在头里：pylib 作者只 include 本头、用下面
// 的宏，不该还需要自己写一遍节声明。
#if defined(_MSC_VER)
#pragma section(".aipyreg$a", read)
#pragma section(".aipyreg$k", read)
#pragma section(".aipyreg$z", read)
#pragma section(".aipyfn$a", read)
#pragma section(".aipyfn$k", read)
#pragma section(".aipyfn$z", read)
#pragma section(".aipybare$a", read)
#pragma section(".aipybare$k", read)
#pragma section(".aipybare$z", read)
#endif

namespace aipy {
namespace pylib {

// 一个模块。注册项本身不带 lookup：函数表由 Function 那一节统一提供，
// 各库不再需要自己写一张「函数名 → 指针」的分派表。
struct Module {
  const char* name;  // 模块名，脚本里 `import text` 的那个 text
};

// 一个导出函数。三件事一起记：它属于哪个模块、叫什么、实现在哪。
//
// 为什么要带模块名：所有库共用同一节，扫描时只能靠这条记录判断归属。
// 这也让「同一个函数名出现在两个模块里」不会互相覆盖。
struct Function {
  const char* module;   // "text" / "threading"
  const char* name;     // "upper" / "start"
  vm::ExtensionFn fn;   // 实现
};

// 一条裸名注册：某个模块的函数在脚本里可以用短名直接调用。
//
//   AIPY_PYLIB_BUILTIN(fs, open, open);   → 脚本里 `open(...)` 就是 `fs.open(...)`
//
// ── 为什么内建表要允许第三方注册 ──
//
// 语言的**内建**（`len` / `print` / `range`）与**扩展**（`text.upper`）在脚本
// 作者看来没有区别：都是「不用 import 就能用的函数」。区别只在实现归属——
// 内建编在 VM 里，扩展由库提供。而 `open` 这类东西的位置本来就在两者之间：
// 它是 Python 的内建，实现却必然依赖平台文件 API，放进核心会让「核心不含
// 平台代码」这条断掉。
//
// 所以让它**以扩展的方式实现、以内建的方式调用**：库提供实现，注册一个裸名，
// 脚本里写 `open(...)` 即可。核心 VM 仍然一行文件代码都没有。
//
// ── 与 builtin 的区别 ──
//
// `len` 永远在；裸名只在这个模块被链接进产物时才在。没链进来时脚本调用它
// 报「扩展函数 open 没有链接进来」——与 `text.upper` 完全一样的措辞，
// 因为对使用者和打包器来说，它们是同一类东西。
struct BuiltinName {
  const char* module;  // 提供实现的模块
  const char* name;    // 该模块里的函数名
  const char* bare;    // 脚本里可以直接写的短名
};

// 按「模块名.函数名」解析。找不到返回 nullptr。
//
// 传入的限定名形如 "text.upper"。为什么用限定名而不是两张表：
// CALL_EXT 只要一个下标，名字里自带模块信息就不用再存一份模块号。
vm::ExtensionFn resolve(const char* qualified);

// 按**裸名**解析：`resolveBare("open")` 找那条声明了「open 就是某个模块
// 函数」的注册项。找不到返回 nullptr。
//
// 与 resolve 分开而不是让 resolve 猜：`open` 里没有点号，而 resolve 的
// 语义是「限定名」。把两种查找混在一个函数里，调用方就得先判断名字形态，
// 而那正是我们希望编译器不必知道的事。
vm::ExtensionFn resolveBare(const char* bare);

// 裸名是否存在（`open` 有没有被任何已链接的库注册）。给启动自检与测试用。
bool bareRegistered(const char* bare);

// 已注册的裸名清单，每条形如 "open <- fs.open"。报错时列「实际有什么」用。
std::vector<std::string> linkedBareNames();

// 已链接进来的模块名，用于启动自检和报错。「扩展函数 text.upper 没有
// 链接进来」比「扩展号越界」有用得多，所以出错时要能列出实际有什么。
std::vector<const char*> linkedModules();

// 扫描得到的模块数量。给测试用：验证「没链接的 pylib 真的一个都没有」。
int linkedModuleCount();

}  // namespace pylib
}  // namespace aipy

// 声明一个模块参与本次链接。
//
//   AIPY_PYLIB_MODULE(text);
//
// 注意展开后是**非 const** 变量。`const Module x` 在 C++ 里是内部链接，
// `/O2` 看见没人引用就直接消除，连节一起没了——编译链接全程零警告，
// 运行时扫描到 0 个。这个坑实测踩过，所以宏里刻意不加 const，并给
// 变量加 external 链接。
#if defined(_MSC_VER)
#define AIPY_PYLIB_MODULE(module_id)                                      \
  AIPY_PYLIB_MODULE_NAMED(module_id, #module_id)

// 符号前缀与对外模块名可以不同。
//
// 需要它是因为 `os.path` 这类点分模块名：对外要叫 `os.path`（脚本里就这么
// 写），而 C++ 标识符里不能有点。规则固定为「符号前缀 = 模块名把点换成
// 下划线」，打包器与链接器用同一条规则，不额外记字段。
#define AIPY_PYLIB_MODULE_NAMED(module_id, module_name)                   \
  extern "C" __declspec(allocate(".aipyreg$k"))                           \
      ::aipy::pylib::Module aipy_pylib_entry_##module_id = {module_name}
#else
#define AIPY_PYLIB_MODULE(module_id)                                      \
  static_assert(false, "pylib 注册目前只实现了 MSVC 的节合并；"           \
                       "其他平台需要各自的分节机制")
#define AIPY_PYLIB_MODULE_NAMED(module_id, module_name)                   \
  static_assert(false, "pylib 注册目前只实现了 MSVC 的节合并；"           \
                       "其他平台需要各自的分节机制")
#endif

// 导出一个函数。
//
//   AIPY_PYLIB_FUNCTION(text, upper, upperImpl);
//
// 展开成符号 `aipy_text_fn_upper`，打包器据此 `/INCLUDE` 精确点名
// （见 tools/aipyic/link.py 的 `_function_symbol`）。
//
// **必须一行一个、且每个单独一个 .cpp**：这是函数级按需的前提，理由见
// 文件头「对标 pybind11」那一节。同一个 .cpp 里写两行，两行的实现在同一个
// obj 里，点一个就全进。
#if defined(_MSC_VER)
#define AIPY_PYLIB_FUNCTION(module_id, function_name, impl)               \
  extern "C" __declspec(allocate(".aipyfn$k"))                            \
      ::aipy::pylib::Function aipy_##module_id##_fn_##function_name = {   \
          #module_id, #function_name, &(impl)}

// 点分模块名（`os.path`）用这个：注册项里的模块名是带点的对外名，
// 符号前缀用下划线版本（`aipy_os_path_fn_exists`）。
#define AIPY_PYLIB_FUNCTION_NAMED(module_id, module_name, function_name,   \
                                  impl)                                   \
  extern "C" __declspec(allocate(".aipyfn$k"))                            \
      ::aipy::pylib::Function aipy_##module_id##_fn_##function_name = {   \
          module_name, #function_name, &(impl)}
#else
#define AIPY_PYLIB_FUNCTION(module_id, function_name, impl)               \
  static_assert(false, "pylib 函数注册目前只实现了 MSVC 的节合并；"       \
                       "其他平台需要各自的分节机制")
#define AIPY_PYLIB_FUNCTION_NAMED(module_id, module_name, function_name,   \
                                  impl)                                   \
  static_assert(false, "pylib 函数注册目前只实现了 MSVC 的节合并；"       \
                       "其他平台需要各自的分节机制")
#endif

// 给一个导出函数起**裸名**，让脚本像用内建一样直接调用。
//
//   AIPY_PYLIB_BUILTIN(fs, open, open);   →  脚本里 `open(p)` == `fs.open(p)`
//
// 三个参数是「模块、模块里的函数名、脚本里写的短名」。前两个必须已经有对应
// 的 AIPY_PYLIB_FUNCTION，否则运行时会注册一条指向空函数指针的裸名。
//
// **必须与对应的 AIPY_PYLIB_FUNCTION 分处不同 .cpp。** 理由和函数注册一样：
// 同一个 .obj 里的符号会被一起抽出。裸名项若与被调用函数同处一个 .obj，
// 「只点名裸名」就会把整个 .obj 拖进来，按需链接在函数粒度上就失效了。
#if defined(_MSC_VER)
#define AIPY_PYLIB_BUILTIN(module_id, function_name, bare_name)           \
  extern "C" __declspec(allocate(".aipybare$k"))                          \
      ::aipy::pylib::BuiltinName aipy_##module_id##_bare_##bare_name = {  \
          #module_id, #function_name, #bare_name}
#else
#define AIPY_PYLIB_BUILTIN(module_id, function_name, bare_name)           \
  static_assert(false, "pylib 裸名注册目前只实现了 MSVC 的节合并；"       \
                       "其他平台需要各自的分节机制")
#endif
