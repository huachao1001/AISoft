# AiPy

Python 语义的 C++ 运行时。**独立项目，不认识任何界面。**

两条设计理念，决定了它长成什么样：

1. **核心虚拟机只支持 Python 语法。** 没有界面、文件、网络、数据库——具体
   能力一律挡在 VM 外面。能力会过时，Python 语义不会。
2. **第三方能力以 `pylib` 进来，与 VM 一起打包成 exe。** C++ 编好的静态库
   （内置的、第三方的）按脚本**实际调用到的**扩展**按需**链接，不全打包；
   交付物是「VM + 用到的 `.lib` + 字节码」一个 exe。

`cl.exe` 留在 pylib 作者那边，打包时只用 `link.exe`。详见
[`AGENTS.md`](AGENTS.md) 的「设计理念」段。

它有两条消费方式，共用同一套语言子集：

| 路线 | 什么时候用 | 产物 |
|---|---|---|
| **字节码 VM**（本项目自带） | 改一行脚本要立刻看到效果 | `.bc` + 解释器 |
| **AOT**（PyUI 的 `pyuic.py`） | 交付、脚本稳定、要性能 | 纯 C++，无解释器 |

两条路线不是竞争关系。「写界面时改一行等十几秒重编译」会把人赶走，所以
开发循环走字节码；脚本定稿后走 AOT，产物里没有解释器。语义只有一份定义，
区别只在最后一步生成什么。

## 为什么它自己一个项目

它要能被**单独拿走**。宿主不一定是 GUI——命令行工具、测试、将来的服务端
都可能需要「Python 语义 + C++ 速度」，而它们不该被迫带一个窗口系统。

编译器（`tools/aipyic/`）也在这里，而且只依赖标准库 `ast`。理由和 CPython
把 `compile` 放解释器里一样：换宿主不该意味着重写编译器；AiPy 被单独拿去
用时，它必须还能往自己肚子里送程序。

这条边界有自动化验证：把同级目录里的 PyUI 整个移开，本项目照样配置、
编译、跑测试全过。

## 构建

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && ctest --output-on-failure
```

需要 MSVC 与 CMake 3.20+。默认 `/MT` 静态 CRT，产物不需要装 VC 运行库。

`-DAIPY_PYTHON=<解释器>` 指向 Python 3.8+：构建期由它编译测试用字节码，
并跑边界守护与语义对拍。不给时会自动找 `python3`/`python`；都没有就跳过
这几条测试并打警告，不影响 C++ 部分的编译。

## 编译并运行一个脚本

```bat
python tools\aipyic\aipyic.py app.py            :: 生成 app.bc
python tools\aipyic\aipyic.py --disasm app.bc   :: 反汇编看指令流
build\aipy_run.exe app.bc                       :: 跑一遍，看脚本输出
build\aipy_vm_dump.exe app.bc                   :: 跑一遍，打印全部状态（JSON）
```

`aipy_run` 与 `aipy_vm_dump` 分开是因为两者的 stdout 含义不同：前者是程序
输出（`print` 打到哪），后者是给测试读的 JSON（语义对拍要解析它）。

`tests/check_vm_semantics.py` 用 `aipy_vm_dump` 跑 VM，同时用 CPython 跑同一
份源码，逐变量比较。期望值不是人写的，而是 CPython 自己的结果——脚本改了
期望值自动跟着变。

## 打包成 exe

交付形态。构建一次，`examples/` 下每个脚本都产出独立 exe：

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAIPY_PYTHON=<python>
cmake --build build
```

`build/examples/` 下得到 `01_basics.exe`、`02_wordcount.exe`、`03_builtins.exe`、
`04_tool.exe`、`05_threads.exe`，前四份各约 328 KB。

单个脚本打包是一行：

```bat
python tools\aipyic\aipyic.py --exe app.py app.exe
```

产物是**单文件**：用户机器上不需要 Python、不需要 MSVC、不需要链接器，
`.bc` 也不用放在旁边。

### 发布：`dist/` 是自包含的

每次构建都会把发布件拷进 `dist/`（Design.md 第 3 节）：

```
dist/
  aipy_app.exe        launcher（纯脚本走这条路，纯文件复制）
  launcher.obj        重链接入口（带扩展的脚本走这条路）
  link_recipe.json    构建期录下的链接器 / obj / 库清单
  aipyic/             编译器与打包器（aipyic.py + compiler/pack/bundle/link/toolchain）
  lib/aipy.lib        语言运行时
  lib/aipy_base.lib
  lib/<模块>_pylib.lib 按需链接的 pylib
```

`dist/` 的持有者不需要 CMake、不需要源码树：

```bat
python dist\aipyic\aipyic.py --exe app.py app.exe ^
    --launcher dist\aipy_app.exe --lib-dir dist
```

带扩展的脚本会自动读 `link_recipe.json` 重链接（不需要 `--build-dir`）。
**唯一的外部依赖是链接器 `link.exe`**（约 15 MB 的 kit，不是整套 MSVC）；
换了机器要重新构建一次 `dist/`，因为配方里记的是本机链接器路径。`dist/`
是派生目录，不入库。

### 用到扩展时（pylib）

脚本 `import` 了某个扩展并真的调用了它，打包时会自动把对应的 `.lib` 链进去：

```bat
python tools\aipyic\aipyic.py --exe app.py app.exe ^
    --build-dir build --lib-dir build
```

- `--build-dir`：CMake 的构建目录。打包器从里面读 `CMakeCache.txt` 与
  `build.ninja`，拿到 `link.exe` 路径和 launcher 的 obj/库清单——不硬编码
  任何路径，换 MSVC 版本也不用改代码。
- `--lib-dir`：`.lib` 所在目录。按 `<模块名>_pylib.lib`（或 `<模块名>.lib`）
  查找。

**按需，精确到函数**：判据是字节码里的**扩展表**（真的调用到的「模块.函数」），
不是 `import` 列表。

- 脚本 `import text` 但一个 `text` 函数都没用 → `text_pylib.lib` 不进产物。
- 脚本只调 `threading.pending()` → `threading_pylib.lib` 会进，但只抽出
  `pending` 所在的 obj；`Thread`/`start`/`waitAll` 的 obj 不会被抽出。

函数级靠 `/INCLUDE:<符号>` 逐个点名。前提是 pylib 侧把每个对外函数做成
独立编译单元——静态库的抽取粒度是 `.obj`，一个 `.obj` 里塞几个函数就会
一起被拉进来。`libs/threading/src/fn_*.cpp` 就是这么分的。

### 内置的 `libs/threading`

线程不是 Python 语义，是具体能力，所以在 `libs/` 下、不进 `aipy.lib`。
模块名与 CPython 的标准库同名，脚本搬过来时 `import threading` 不用改。

```python
import threading

def square(n):
    return n * n

t = threading.Thread("square", 7)   # target 写函数名，不是函数本身
t.start()
t.join()
print(t.is_alive())                 # False

values = []                         # 要收集返回值用 waitAll（AiPy 扩展）
threading.start("square", 9)
values = threading.waitAll()
print(values)                       # [81]

lock = threading.Lock()
with lock:                          # __enter__ 加锁、__exit__ 解锁
    print("protected")
```

接口与标准库对齐到这一步：

| AiPy | CPython `threading` | 说明 |
|---|---|---|
| `threading.Thread(name, ...)` | `Thread(target=..., args=...)` | **target 写函数名**：AiPy 没有一等函数，也不支持关键字传参 |
| `t.start()` | `t.start()` | 重复 start 返回 None 且不再提交（CPython 抛 RuntimeError） |
| `t.join()` | `t.join()` | 无参、等自身完成 |
| `t.is_alive()` | `t.is_alive()` | |
| `t.name()` | `t.name` | 方法是 `name()`（没有属性访问） |
| `threading.Lock()` | `Lock()` | `acquire()` / `release()` / `with` |
| `threading.Event()` | `Event()` | `set()` / `clear()` / `is_set()` / `wait()` |
| `threading.current_thread()` | `current_thread().name` | 直接返回名字字符串 |
| `threading.active_count()` | `active_count()` | 含主线程；空闲时是 1 |
| `threading.start(name, ...)` | — | AiPy 扩展：一行提交，返回任务号 |
| `threading.waitAll()` | — | AiPy 扩展：等全部完成并收结果列表 |
| `threading.poll()` / `pending()` | — | AiPy 扩展：非阻塞取结果 |

九个导出函数各自独立成 obj，按需链接。

**为什么 target 是名字**：AiPy 里函数不是值（`Value` 没有函数类型，
`g = f` 编译不过），所以 `Thread(target=worker)` 这种写法不存在。这是语言
事实，不是 API 取舍。

**`with` 的语义是完整的**：正常走完、`return`、循环里 `break`/`continue`，
四条路径都会调 `__exit__`。漏一条的症状不是报错，是「第二次进锁永久
阻塞」——`aipy_threading` 测试专门验这一点。

**线程间共享数据**：全局变量是**进程级共享**的，子线程里 `global TOTAL`
写的就是主线程看得见的那个槽（与 Python 的模块全局变量一致）。`Value` 的
引用计数是原子的，所以两个线程各持同一个 list 的句柄是安全的。但「两个
线程同时 append 同一个 list」或「同时改同一个全局变量」仍是数据竞争——
引用计数管的是生命周期，不是互斥，那要用 `threading.Lock()`。

写一个 pylib 只要两步，用的是与 `libs/threading` **完全相同**的两条宏：

```cpp
// 一个文件声明模块
#include "aipy/pylib.h"
AIPY_PYLIB_MODULE(text);

// 每个函数一个文件（函数级按需的前提，见下）
#include "aipy/pylib.h"
static aipy::Value upperImpl(const aipy::Value* args, int count) { /* ... */ }
AIPY_PYLIB_FUNCTION(text, upper, upperImpl);
```

与 pybind11 的对照：

| pybind11 | AiPy |
|---|---|
| `PYBIND11_MODULE(text, m)` | `AIPY_PYLIB_MODULE(text);` |
| `m.def("upper", &upper)` | `AIPY_PYLIB_FUNCTION(text, upper, upperImpl);` |

**唯一的差别：函数注册必须一个函数一个 `.cpp`。** pybind11 可以把一堆
`m.def()` 写在模块块里，因为整个模块编成一个 `.so`；AiPy 要求函数级按需
（Design.md 第 1.3 条），而静态库的抽取粒度是 `.obj`——几行写在一个 obj
里，`/INCLUDE` 点到其中一个就把其余实现一起拉进产物。这条差别来自交付
形态，不是 API 设计失误，`include/aipy/pylib.h` 文件头有完整推导。

系统库与用户自定义库用同一套，因此 `tests/pylib/`（`text`，验证按需链接）
就是一份可直接照抄的样板，没有各库私有的 `registry.h`。

`AIPY_PYLIB_MODULE` 展开后**必须非 `const`**——否则 `/O2` 看见没人引用就
把整节消除，编译链接零警告、运行时扫到 0 个。这个坑实测踩过，原因写在
`include/aipy/pylib.h` 的注释里。

**打包不编译任何 C++**：把字节码追加到预编译 launcher 的尾部，再写 16 字节
尾部索引（`magic "AIPYAPP1" + 长度 u64`）。Windows 加载器只按节表映射镜像，
尾部多余字节不参与加载，所以不需要资源段、不需要内置解压，整个打包是纯
Python 的文件复制。用到扩展时多一步 `link.exe` 拼装，但那是链接成品 `.lib`，
不是编译源码。

为什么不把脚本编成 C++ 数组再编译：那要求每次改脚本都重新编译链接，而用户
机器上只有 `link.exe`，甚至什么都没有。所以 **AiPy 的应用开发是纯 Python**：
C++ 只是那个 325 KB 的 launcher，写一次、编译一次，之后所有人只管写 `.py`。

### 这 325 KB 是什么

用 `dumpbin /MAP` 按符号地址差统计出来的实际构成：

| 占比 | 大小 | 内容 |
|---|---|---|
| 34.5% | 78 KB | C 运行库 + CRT 启动（静态 `/MT`，含 `printf`/`malloc`/locale） |
| 33.5% | 76 KB | AiPy 运行时 + launcher（其中 `value.cpp` 37 KB） |
| 23.5% | 53 KB | C++ 模板实例（`vector`/`string` 的 reserve/replace/find） |
| 8.1% | 19 KB | VCRuntime（异常展开、栈帧、`memcpy`） |

**你自己写的代码只占 76 KB**，剩下的都是「不依赖用户装 VC 运行库」的代价——
`/MT` 把 CRT 静态链进来了。这是产物能单文件交付的前提，不是浪费。

一处实测出来的教训：launcher 最初用 `std::ifstream` + `istreambuf_iterator`
读文件，那套代码会把 `basic_ifstream`、`codecvt`、`use_facet` 和整套格式化
机器拖进来，**多出 68 KB**（393.5 → 325.5 KB）。改成 `fopen`/`fread` 就没了。

> **launcher 里不许用 iostream。** 读文件用 C API。`#include <fstream>`
> 或 `<sstream>` 会连带拉进几十 KB 的模板实例，而 launcher 用不到 locale
> 和格式化。

同一个问题现在还在 `aipy_run.exe`（387 KB）和 `aipy_vm_dump.exe`（389.5 KB）
里——它们仍用 `ifstream`，比 launcher 大 60 KB 左右。这两个是开发工具、不是
交付产物，所以先留着；哪天要瘦身，照 launcher 的改法抄一遍就行。

进一步压缩的空间已经很薄：`/LTCG` 与 `/OPT:ICF` 实测都是 0 收益（库是
`/GL-` 编译的，没有 MSIL 可优化）。要再小只能自己写浮点解析和部分 CRT
函数替换，那是拿正确性风险换几十 KB，不划算。

调试用的两条：

```bat
python tools\aipyic\aipyic.py --unbundle app.exe app.bc  :: 取回字节码
python tools\aipyic\aipyic.py --disasm app.bc            :: 反汇编
```

`examples/` 下有四份纯 Python 示例，全部会打包成 exe 并实跑验证。见
`examples/README.md`。

## 目录

```
include/aipy/types.h     不透明宿主指针 Host
include/aipy/utf8.h      UTF-8 字符边界扫描
include/aipy/value.h     Value 与全部内建
include/aipy/builtins.h  内建函数表（编译期下标 ↔ 运行期函数指针）
include/aipy/vm.h        操作码、Program、Machine（公开接口）
include/aipy/bytecode.h  字节码读取：独立 .bc 文件 + 打包产物内嵌脚本
include/aipy/pylib.h     pylib 注册契约（节合并 + AIPY_PYLIB 宏）
src/value.cpp
src/utf8.cpp
src/builtins.cpp
src/bytecode.cpp         .bc 加载器
src/pylib.cpp            扫描已链接的 pylib 模块
src/vm/internal.h        VM 的内部契约（不属于公开 API）
src/vm/machine.cpp       执行循环（唯一的大 switch）
src/vm/operators.cpp     算子分派
src/vm/methods.cpp       方法分派（字符串/列表/字典）
src/vm/names.cpp         操作码命名
src/vm/disasm.cpp        反汇编
src/vm/state.cpp         热加载的状态搬运

libs/threading/include/aipy/threading/threading.h  线程库对外说明与边界
libs/threading/include/aipy/threading/registry.h   函数级注册（AIPY_THREADING_FN）
libs/threading/include/aipy/threading/pool.h       池与可等待结果（共用底座）
libs/threading/src/pool.cpp                        池、Future、按名字提交
libs/threading/src/fn_thread.cpp                   每个导出函数一个编译单元
libs/threading/src/fn_event.cpp
libs/threading/src/fn_lock.cpp
libs/threading/src/fn_start.cpp
libs/threading/src/fn_poll.cpp
libs/threading/src/fn_pending.cpp
libs/threading/src/fn_wait_all.cpp
libs/threading/src/fn_active_count.cpp
libs/threading/src/fn_current_thread.cpp
libs/threading/src/module.cpp                      模块级注册（AIPY_PYLIB）

tools/aipyic/compiler.py 脚本 → 字节码（只用标准库 ast）
tools/aipyic/pack.py     .bc 二进制格式（权威定义在其头部）
tools/aipyic/bundle.py   把字节码接到 launcher 尾部（打包成 exe 的格式）
tools/aipyic/link.py     按需链接 pylib（读 CMake 配方，调 link.exe）
tools/aipyic/aipyic.py   命令行入口：编译 / 反汇编 / 打包
tools/aipy_app/          打包成 exe 的执行端（launcher）
tools/aipy_run/          跑脚本、print 打到 stdout
tools/aipy_vm_dump/      跑字节码、把全局状态打成 JSON
tools/check_no_ui.py     边界守护（CTest: aipy_layers）
examples/                五份纯 Python 示例（每份都打包成 exe）
tests/test_standalone.cpp 只链 aipy 的程序（独立性的活证据）
tests/test_value.cpp     41 个语义用例，每条对应一句 CPython 行为
tests/test_vm.cpp        VM 端到端：加载字节码、执行、断言结果与行号
tests/scripts/           端到端用例的 Python 源码
tests/build_bytecode.py  生成测试字节码，同时校验两边操作码表一致
tests/check_vm_semantics.py 与 CPython 逐变量对拍
tests/check_examples.py  示例与 CPython 比对 + 打包产物实跑
tests/probe_syntax.py    语言支持清单的实测探针（186 条）
LANGUAGE.md              支持哪些 Python 语法，逐条实测（186 条）
```

## VM 为什么分这么多文件，执行循环却不拆

`src/vm/` 按职责分文件，唯独 `Machine::run` 那 350 行的 switch 留在原地。
这不是懒得拆，两个方向各有硬理由：

**拆出去的是语义**，它们与「怎么循环」无关，而且改它们的理由各不相同：
算子（`operators.cpp`）改是因为 `Value` 的语义变了；方法（`methods.cpp`）
改是因为要加一个可调用方法；反汇编（`disasm.cpp`）改是因为要给操作数
加一种编码。混在一个文件里，任何一处改动都要重读整份执行循环。

**留在一起的是分派**，因为每条指令再多一次函数调用，就否定了这一层存在
的理由——477 ms / 300 万次累加里，分派本身占了可观份额。拆成「每条指令
一个函数」是教科书做法，但那是给可读性付性能账，而这里恰好不能付。

`internal.h` 里的 `Frame` 刻意不放进 `aipy/vm.h`：它是执行循环的私有数据，
让宿主看见就等于承诺永不变，而这个结构会随指令集调整。

## 十一条测试

| 测试 | 作用 |
|---|---|
| `aipy_tests` | 41 个 Value 语义用例，对着 CPython 行为 |
| `aipy_standalone` | 只链 `aipy`，同时是独立性的回归测试 |
| `aipy_vm_tests` | 字节码端到端：算术、循环、函数、方法、切片、错误行号、热加载搬运 |
| `aipy_disasm` | 反汇编器与 C++ 加载器各自解码同一份 `.bc` |
| `aipy_examples` | 示例与 CPython 逐行比对；五份示例全部打包成 exe 实跑 |
| `aipy_activity` | `class X(Activity)` 展开路径：字段、增强赋值、方法前向调用 |
| `aipy_pylib` | 按需链接：链了能调、没链报错、不用扩展的产物里无 pylib 符号 |
| `aipy_threading` | 线程库：`Thread`/`Event` 对齐、函数级按需、`with` 完整语义、提交时校验 |
| `aipy_vm_semantics` | VM 结果与 CPython 逐变量一致 |
| `aipy_layers` | 边界守护：无界面符号、`Host` 是不透明 `void*` |
| `aipy_standard` | Design.md 第 4 节：系统库与用户库同一套宏、平台细节集中在 `toolchain.py` |

字节码是**构建期生成**的派生物，不入库。曾经手动放在源码树里，结果改完
脚本忘了重新生成，测试对着旧字节码报了两次假失败。

## 三个设计约束

### 1. 零界面依赖

不许出现 `View`/`Window`/`Painter`/`HWND` 这类符号。需要持有宿主对象时
用 `Host`（`void*`）——原样存、原样带回去，不解释。

`Value` 里那个 `kHost` 类型就是为此存在的：宿主（比如 PyUI）放一个
`View*` 进去，取出来自己转回 `View*`。AiPy 全程不知道它是什么。

### 2. `Value` 必须是 16 字节

它被每次表达式求值拷贝。实测：96 字节时 300 万次循环 805 ms，16 字节时
1.11 ms。所以不能有 `std::string` 成员（32 字节）、不能有 `std::shared_ptr`
（16 字节）。堆载荷走侵入式引用计数。

### 3. 语义对齐 CPython，不是「更合理」

每个内建的行为都以 CPython 的可观察结果为准。`" ".join([1, 2])` 在 Python
里抛错，如果这里改成静默转字符串，脚本迁回 CPython 时就行为不一致——
兼容性承诺是这个项目存在的全部意义。

**支持的是一个子集，不是全部 Python。** 逐条实测的清单在
[`LANGUAGE.md`](LANGUAGE.md)：184 条写法，125 条支持、59 条被
明确拒绝。改完编译器用 `python tests\probe_syntax.py build\final\aipy_run.exe`
重跑，别靠记忆判断。

## 操作码与内建表是两份实现之间的契约

操作码编号在 `vm.h` 的 `enum class` 与 `compiler.py` 的 `OPCODES` 里各有一
份；内建顺序在 `builtins.cpp` 的 `kBuiltinTable` 与 `compiler.py` 的
`BUILTINS` 里各有一份。错一位的后果不是崩溃，而是「所有指令集体错位、
结果莫名其妙」。

所以 `tests/build_bytecode.py` 每次构建都解析 `vm.h` 与 `builtins.cpp`，
和 Python 侧逐条对拍，不一致就构建失败并指名第几条。

## 跨平台：哪些是平台细节，哪些不是

VM、字节码、编译器、`Value` 语义都与操作系统无关。与平台绑定的只有三层，
它们各自收在一处，加一个平台不需要动语言实现：

| 层 | 位置 | 目前实现 | 换平台要做什么 |
|---|---|---|---|
| pylib 注册 | `include/aipy/pylib.h` + `src/pylib.cpp` | MSVC 节合并 | GNU ld 用 `__attribute__((section))` + `__start_/__stop_` |
| 链接器调用 | `tools/aipyic/toolchain.py` | `msvc`（`link.exe /INCLUDE:`） | 实现一个新的 `Toolchain` |
| launcher 读自身 | `tools/aipy_app/main.cpp` 的 `readSelf` | `GetModuleFileNameW` | `readlink("/proc/self/exe")` |

`pylib.cpp` 在非 MSVC 上提供**同一组函数**（返回空表），所以核心库能编、
`aipy_standalone` 能跑；而**用宏的库**在非 MSVC 上编译失败而不是静默空表
——静默空表的症状是「打包成功、运行时报扩展没链接」，比编译错误难查。

`aipy_standard` 测试守着这条边界：`link.py` 里不许出现写死的链接器参数，
不支持的平台必须报 `ToolchainError` 而不是崩溃。

## 与 PyUI 的关系

PyUI 通过 `add_subdirectory(${CMAKE_SOURCE_DIR}/../AiPy ...)` 消费本项目的
`aipy` 与 `aipy_base` 两个目标。**本项目不反向依赖 PyUI**，这是单向的。

两个项目必须并排放置：

```
<parent>/AiPy     ← 本项目
<parent>/PyUI     ← 消费者
```

「不反向依赖」有实测：把 `PyUI` 改名藏起来，`cmake` + 构建 + `ctest` 全过。

## 分发出的库必须关 `/GL`

`/GL` 的产物里是 MSIL，链接期要调 `c2.dll` 重新生成机器码。用户机器上
只有 `link.exe`，所以 `aipy` / `aipy_base` 都显式 `/GL-`。可执行文件
（`aipy_tests`、`aipy_standalone`、`aipy_vm_dump`）不受这条限制。
