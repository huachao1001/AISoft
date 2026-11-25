# AiPy 开发约定

Python 语义的 C++ 运行时。开头是设计理念（为什么是这个形态），后面是
**必须遵守的铁律**（具体怎么守）。

## 设计理念

两句话说明 AiPy 是什么。后面所有铁律都是从这两条推出来的。

**一、核心虚拟机只支持 Python 语法。**

`src/` 里的 VM 不认识任何具体能力——没有界面、没有文件、没有网络、没有
数据库。它只做三件事：读字节码、按 Python 语义执行、管理值。问它「怎么画
按钮」「怎么读文件」，它一律不知道，也不该知道。

这不是为了架构好看，是它唯一能长期活下去的形态：具体能力会过时（GUI 框架
会换、协议会换），而 Python 语义不会。能力挡在 VM 外面，VM 就不用跟着它们
一起改。

**二、第三方能力以 `pylib` 进来，与 VM 一起打包成 exe。**

C++ 已经编译好的静态库（内置的、第三方提供的）统称 `pylib`——不用 `par`
这个叫法。交付时把「VM + 用到的 pylib + Python 字节码」链成一个 exe：

```
应用 exe = VM + 用到的 pylib(.lib) + 脚本字节码(.bc) + 尾部索引
```

**按需，不全打包——而且精确到函数。** 只做字符串处理的应用不该把整套 GUI
拖进去。打包时按脚本的**扩展表**（真的调用到的「模块.函数」，不是 import
列表）决定链什么：库没用到就不进产物；库用到了，也只把**真的调用到的
函数**拉进来。

函数级能做到，是因为静态库的抽取粒度是 **`.obj`**：pylib 侧每个对外函数
独立成一个编译单元（`libs/threading/src/fn_*.cpp`），打包时用
`/INCLUDE:<符号>` 逐个点名。脚本只用 `threading.pending`，`Thread`/
`start`/`waitAll` 的 obj 连抽都不会被抽出——实测：不点名时注册表里函数全查
不到，只点名 `pending` 时只有 `pending` 能被查到。

**边界：`cl.exe` 在作者那边，`link.exe` 在用户这边。**

| 阶段 | 谁做 | 用什么 |
|---|---|---|
| 写 `pylib` | pylib 作者，一次性 | `cl.exe` 编成 `.lib` |
| 打包应用 | 终端用户 | `link.exe` 链 `.lib` + 追加字节码 |

「不编译 C++」指的是**不把 `.cpp` 编成 `.o`**。`.lib` 是拿来的成品，链接
它不算编译。用户机器上没有 `cl.exe` 也不需要，只要有链接器（约 15 MB 的
kit）。这两句话看起来矛盾，其实是同一件事的两面，见铁律九。

**现状**：这条已经跑通了。`aipyic.py --exe` 检测到脚本用到扩展时，会读
CMake 的构建配方、用 `link.exe` 把 launcher 的 obj + 选中的 pylib 重新链
一遍，再追加字节码。机制与验证见下节。

### pylib 怎么注册到 VM

静态库有个天然麻烦：链接器只把「被引用到的」obj 从 `.lib` 里抽出来，而
注册项没人显式引用，会被整体丢弃。这里用节合并解决，两段节各管一层：

```
.aipyreg$a 哨兵（aipy.lib 提供）      .aipyfn$a 哨兵
.aipyreg$k 各模块（AIPY_PYLIB_MODULE）  .aipyfn$k 各函数（AIPY_PYLIB_FUNCTION）
.aipyreg$z 哨兵                        .aipyfn$z 哨兵
```

链接器把同名节按 `$` 后的字母序合并，主程序运行时扫描这两段连续内存，就
发现所有**被链接进来的**模块与函数。不需要初始化函数、不依赖静态构造
顺序、打包期也不生成任何 C++ 胶水代码。

两个坑（都实测踩过，写进了 `include/aipy/pylib.h` 的注释）：

- **注册对象不能是 `const`**。`const Module x` 是内部链接，`/O2` 看见
  没人引用就整体消除，连节一起没了——编译链接零警告，运行时扫到 0 个。
- **注册项所在的 obj 必须被拉进来**。模块级用 `/WHOLEARCHIVE`，函数级用
  `/INCLUDE:<符号>`；两者都不做，产物里一个模块都扫不到。

### 函数级注册：一套宏，所有库共用

`libs/threading`（系统库）与 `tests/pylib/`（用户自定义库样板）用的是
**完全相同**的两条宏，没有各库私有的 `registry.h`：

```
AIPY_PYLIB_MODULE(threading)                   → 模块级：这个库参不参与
AIPY_PYLIB_FUNCTION(threading, start, impl)    → 函数级：哪个函数进产物
```

对标 pybind11：`AIPY_PYLIB_MODULE` 对应 `PYBIND11_MODULE`，
`AIPY_PYLIB_FUNCTION` 对应 `m.def()`。唯一差别是**每个函数必须单独一个
`.cpp`**——pybind11 把 `m.def()` 写在模块块里没事（整个模块一个 `.so`），
而静态库的抽取粒度是 `.obj`，几行写在一个 obj 里就一起被拉进来，函数级
按需失效。

模块级只解决「这个库参不参与」；函数级解决「库里的哪个函数进产物」。
两层都要：缺模块级，加载器报「扩展函数 threading.start 没有链接进来」；缺
函数级，模块在但函数查不到。

`aipy_standard` 测试守着这条：两个库的注册行必须同形、不许有私有 registry。

PyUI 将来就是这样一个 `pylib`：它自带 `pyui_core`/`pyui_script`，被 AiPy
当作外部能力消费；反过来说，AiPy 永远不认识界面。

### `libs/` 放什么

`libs/` 下每个目录是一个独立 pylib，**不进 `aipy.lib`**。判断标准很简单：
**它是 Python 语义，还是具体能力？** 语义留在 `src/`；能力进 `libs/`。
线程、文件、时间、JSON、网络、界面全属于后者。

现有四个：`threading`（线程）、`fs`（文件）、`time`（时间）、`json`（数据
交换）。它们与用户自定义库用的是**同一套** `AIPY_PYLIB_MODULE` /
`AIPY_PYLIB_FUNCTION`，没有私有注册代码——加第五个库只需要新目录 + 两条宏。

线程刻意只提供**按名字跑脚本函数**：AiPy 里函数不是值（`Value` 没有
函数类型，`g = f` 编译不过），所以没有 `Thread(target=worker)` 这种写法
可选，只能写 `Thread("worker", 参数...)`。这是语言事实，不是 API 取舍。

### 带方法的对象（`kObject`）

`threading.Lock()` 必须能调 `acquire()`/`release()`，而 `kHost` 只是「原样存、
原样带回去」的哑指针。所以 `Value` 多了一种类型 `kObject`：一堆堆载荷
（`ObjectData`）带一个 `host`、一张方法表、一个析构回调。

**VM 仍然不认识任何具体能力**：它只会「按方法名查表转发」，表由 pylib 提供。
加一种新对象（文件句柄、网络连接）不需要改 `src/` 一行。

`with` 就是建立在这上面的：`with lock:` 调的是表的 `__enter__`/`__exit__`。
语言里没有可实例化的类（只有 `class X(Activity)`），所以 manager 只能来自
pylib——这不是限制，是「一切皆是第三方库」的直接推论。

`__enter__` 返回的是**接收者 Value 本身**（`With lock as l:` 的 `l` 就是锁）。
方法签名收 `const Value& self` 而不是 `ObjectData*`：用 host 重新包一个 Value
会造出第二份堆载荷，两份各自 `delete` 同一个 host，结果是 double free。
这个坑踩过。

## 铁律一：不认识界面

`include/aipy/`、`src/` 下任何文件都不许出现 `View`、`Window`、`Painter`、
`Surface`、`HWND`、`HDC`、`Theme`，也不许 include 任何 `pyui/` 的头。

需要持有宿主对象时用 `Host`（`void*`）：原样存、原样带回去，不解释它
指向什么。`Value::asHost()` 是唯一出口，把它转成什么类型是宿主的事。

这条边界由**本项目自己的** `tools/check_no_ui.py` 守着（CTest 里的
`aipy_layers`）。检查留在 AiPy 而不是只交给消费方：AiPy 被整个复制到另一个
仓库时，那份消费方的检查根本不在，而那正是最需要保护的时刻。

违反的症状是「AiPy 单独发布时突然拖进一整套图形代码」——那种退化在编译期
完全看不出来，只有真的去切一次包才会发现。

## 铁律二：`Value` 是 16 字节，不许长大

它被每次表达式求值拷贝。实测代价：96 字节 → 300 万次循环 805 ms；
16 字节 → 1.11 ms。

- 不许加 `std::string` 成员（它一个人 32 字节）
- 不许加 `std::shared_ptr` 成员（它一个人 16 字节）
- 容器与字符串的载荷统一放 `HeapData`，由侵入式引用计数管理

加任何成员之前先跑一次体积检查。

**引用计数用 `std::atomic`，不许改回裸整数。** 这是对象回收机制自身的
正确性，不是「支持多线程的代价」：非原子计数只在「所有持有者恰好同线程」
时正确，而这条约定没有编译期或运行期检查。两个线程各持一个 list 句柄时，
`--refs` 竞争会让它提前归零（用已释放内存）或永不归零（泄漏），症状远离
出错点。`refs` 是 4 字节、`atomic<uint32_t>` 也是 4 字节，`Value` 仍 16
字节；实测代价在噪声级（容器句柄拷贝 +0.2%，算术求值 0%）。

## 铁律三：语义对齐 CPython，不是「更合理」

每个内建的行为都以 CPython 的可观察结果为准。`tests/test_value.cpp` 里
每条用例的注释都写出对应的 Python 代码。

「这样设计更安全」不是理由。真实反例：`" ".join([1, 2])` 在 Python 里抛
错，但如果这里改成静默转字符串，脚本迁回 CPython 时就会行为不一致——
而兼容性承诺是这个项目存在的全部意义。

## 铁律四：分发出的库关 `/GL`

`/GL` 产物是 MSIL，链接期要 `c2.dll`。用户机器上只有 `link.exe`。
`aipy` 与 `aipy_base` 必须显式 `/GL-`（见 `CMakeLists.txt`）。

## 铁律五：编译器与加载器属于本项目，不属于宿主

`tools/aipyic/`（脚本 → 字节码）与 `src/bytecode.cpp`（字节码 → 程序）
都在这里，且只依赖标准库。理由和 CPython 把 `compile`/`marshal` 放进
解释器一样：换宿主不该意味着重写编译器。

这条被真实踩到过：最初把编译器放在 `PyUI/tools/pyuic/`，构建期引用那一
路径。把 `PyUI` 改名藏起来验证独立性时，`cmake` 直接报
`ninja: error: '.../PyUI/tools/pyuic/vm_compiler.py' missing`——语言项目
因为「编不了自己的语言」而不能独立构建。

判断标准：**这个文件在「把 AiPy 单独拷到另一个仓库」后还需要吗？**
需要就属于这里。

这条只约束**语言本身**（编译器、加载器、VM）。具体能力不适用——那些是
`pylib`，本来就该在别的仓库（见「设计理念」第二条）。

## 铁律六：执行循环不拆，语义拆

`src/vm/` 按职责分文件（算子、方法、命名、反汇编、热加载），但
`Machine::run` 那个大 switch 必须留着。

拆成「每条指令一个函数」会让每条指令多一次调用与返回，而这一层的全部
理由就是「一次 switch + 几次直接下标」。477 ms / 300 万次累加的基准里，
分派本身占可观份额。

判断标准：**改动它是因为「语言语义变了」还是「指令怎么执行」？**
前者拆出去（`operators.cpp`/`methods.cpp`），后者留在 `machine.cpp`。

`internal.h` 里的 `Frame` 不放进 `aipy/vm.h`：它是执行循环的私有数据，
让宿主看见等于承诺永不变。

## 铁律七：示例必须能跑，而且要有测试

`examples/` 下每加一个 `.py`，`CMakeLists.txt` 的 `file(GLOB)` 会自动把它
打进 `build/examples/<stem>.exe`，`aipy_examples` 也会自动纳入比对。

两条都要满足：

1. 能写成纯 Python 子集的，与 CPython 输出逐行比对
2. 每一份都打包成 exe 实跑一遍，输出必须与脚本一致

第 2 条不是重复劳动：尾部索引写错时字节码本身完全正确，坏的是「怎么读
回来」，只有真的跑一次 exe 才发现。

示例过时的受害者是照着它写代码的新用户，而他们不会去读源码确认哪边错了。

## 铁律八：示例里不许出现 C++，应用开发是纯 Python

`examples/` 下只放 `.py`。要演示的能力就用脚本演示，不要写一个
`main.cpp` 来「展示怎么用 C++ 调 AiPy」——那会把「AiPy 是把 Python
打成 exe 的工具」说成「AiPy 是个 C++ 库」。

这条踩过：一开始 `examples/embed/` 里有 `main.cpp` + 两个 `.py`，用来
演示热加载。方向是错的——它教的是宿主用法，而 AiPy 的用户模型是
「写 .py，打成 exe」。现在四份示例都是 `.py`，构建时自动产出对应 exe。

`tools/` 里的 C++ 是 launcher 与测试，不是示例。

## 铁律九：打包不编译 C++

`aipyic.py --exe` **不得**引入「生成 `.cpp` 再 `cl.exe` 编译」的路径。

理由：那条路要求每次改脚本都重新编译链接，而用户机器上只有 `link.exe`。
一旦为了「打包方便」把编译器拉回打包链路，整个交付模型就退回
「用户装一套 MSVC」。

判断标准：**打包时用到 `cl.exe` 了吗？** 用到了就是错的方向。

注意边界：**链接 `pylib` 的 `.lib` 不算编译**（见「设计理念」第二条）。
打包用到扩展时确实会调 `link.exe` 把 `.lib` 链进 launcher——那是拼装
成品，不是编译源码。今天的实现就是这么做的（`tools/aipyic/link.py`），
它读 CMake 的构建配方来拿 `link.exe` 路径与库清单，不硬编码任何路径。

## 铁律十：launcher 里不许用 iostream

`tools/aipy_app/` 读文件用 `fopen`/`fread`，**不许**用 `std::ifstream` +
`istreambuf_iterator`，也不许 include `<fstream>` / `<sstream>`。

实测代价：那套代码把 `basic_ifstream`、`codecvt`、`use_facet` 与整套格式化
机器拖进产物，多出 **68 KB**（393.5 → 325.5 KB）。launcher 只需要「把一个
文件读进内存」，用不到 locale、用不到格式化。

同一个问题在 `aipy_run.exe` / `aipy_vm_dump.exe` 里还在，但它们是开发工具、
不是交付产物，可以容忍。**launcher 是唯一会被复制进每个应用 exe 的二进制**，
所以约束只加在它身上。

## 注释风格

- 中文，只说「为什么」
- 不复述代码
- 踩过的坑要写清症状，那比结论有用

## 构建与测试

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && ctest --output-on-failure
```

十二条测试：

| 测试 | 作用 |
|---|---|
| `aipy_tests` | 41 个语义用例，对着 CPython 行为 |
| `aipy_standalone` | 只链 `aipy` 的程序，同时是独立性的回归测试 |
| `aipy_vm_tests` | 字节码端到端：循环、函数、方法、切片、错误行号、状态搬运 |
| `aipy_disasm` | 反汇编器与 C++ 加载器各自解码同一份 `.bc` |
| `aipy_examples` | 示例与 CPython 逐行比对；六份示例全部打包成 exe 实跑 |
| `aipy_activity` | `class X(Activity)` 展开路径：字段、增强赋值、方法前向调用 |
| `aipy_pylib` | 按需链接：链了能调、没链报错、不用扩展的产物里无 pylib 符号 |
| `aipy_threading` | 线程库：`Thread`/`Event` 对齐、函数级按需、`with` 完整语义、提交时校验 |
| `aipy_system_libs` | `fs`/`time`/`json` 三库：功能、错误返回值约定、各自的函数级按需 |
| `aipy_vm_semantics` | VM 结果与 CPython 逐变量一致 |
| `aipy_layers` | 边界守护：无界面符号、`Host` 是不透明 `void*` |

`aipy_standalone` 不只是演示：它断言了算术、字符串内建、容器引用语义、
UTF-8 字符下标等行为。哪天某个内建悄悄改了行为，它会红。

`aipy_vm_semantics` 与 `aipy_examples` 的期望值不是人写的，而是 CPython
自己跑出来的结果。手写的期望值写错就永远绿，保护不了任何东西。

跑测试需要 Python 3.8+（构建期与测试期，运行期不需要）：构建期要用它把
`tests/scripts/*.py` 编成字节码，还要跑边界守护与语义对拍。不给
`-DAIPY_PYTHON=...` 时按 `python3` / `python` 查找；都找不到就跳过这几条并
打警告，不影响 C++ 部分的编译。

字节码是派生物，**不入库**、在构建期生成。曾经手动放在源码树里，结果改完
脚本忘了重新生成，测试对着旧字节码报了两次假失败。
