# 示例

每个示例只教一件事，都是能跑的完整脚本——不是片段。
**全部是纯 Python，没有 C++ 文件。**

## 先跑起来

```bat
build\pcc.exe examples\01_basics.py build\01.bc
build\aipy_run.exe build\01.bc
```

`pcc.exe` 把脚本编成字节码，`aipy_run.exe` 执行它。两步都能单独看：
`pcc.exe --disasm build\01.bc` 显示生成的指令流。

## 打包成 exe

这是 AiPy 的交付形态。构建一次就自动产出 7 个 exe：

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAIPY_PYTHON=<python>
cmake --build build
```

产物在 `build/examples/`：

```
build\examples\01_basics.exe       556 KB
build\examples\02_wordcount.exe    554 KB
build\examples\03_builtins.exe     557 KB
build\examples\04_tool.exe         556 KB
build\examples\05_threads.exe      582 KB（含 threading 库）
build\examples\06_files.exe        645 KB（含 fs/os/os.path/json/time 五个库）
build\examples\07_interactive.exe  556 KB（含 fs 库，演示 input）
```

每个都是**单文件**：双击或在任何目录运行都行，不需要 Python、不需要 MSVC、
不需要 `.bc` 放在旁边。

单个脚本打包也是一行：

```bat
build\pcc.exe --exe examples\04_tool.py build\invoice.exe
```

### 打包在做什么

把字节码追加到预编译 launcher 的尾部，再写 16 字节尾部索引：

```
[aipy_app.exe 原始字节][字节码][magic "AIPYAPP1" + 长度 u64]
```

Windows 加载器只按节表映射镜像，尾部多余字节不参与加载，所以不需要资源段，
也不需要任何编译器参与打包。**打包全过程是文件复制。**

为什么不把脚本编成 C++ 数组再编译：那要求每次改脚本都重新编译并链接，而
用户机器上只有 `link.exe`。那条路会把「改一行脚本」变成「装一套 MSVC」。

### 两个附带命令

```bat
:: 从打包产物里取回字节码（调试用）
build\pcc.exe --unbundle build\invoice.exe build\back.bc
:: 反汇编看指令流
build\pcc.exe --disasm build\back.bc
```

`--exe` 默认自动找最新构建出来的 `aipy_app.exe`；换构建目录时用
`--launcher` 显式指定，免得用着旧 launcher 却以为是新脚本的问题。

## 清单

| 文件 | 教什么 | 能在 CPython 里跑吗 |
|---|---|---|
| `01_basics.py` | 变量、算术、条件、循环、函数、列表、字典、切片、中文按字符处理 | 能，输出逐字节一致 |
| `02_wordcount.py` | 一个真能干活的小工具：切词、字典累加、打擂台找最大、分组 | 能，输出逐字节一致 |
| `03_builtins.py` | 18 个内建里的实用部分，含 AiPy 独有的三个 | 不能（用了独有内建） |
| `04_tool.py` | 可交付的小工具：满减/折扣/税额/运费，**打包成 exe 的示例** | 能，输出逐字节一致 |
| `05_threads.py` | `Thread`/`join` + `with lock:` 保护共享状态，**用到 pylib** | 不能（`Thread` 第一个参数是函数名） |
| `06_files.py` | `fs` 读写文件、`json` 存取结构、`time` 计时，含 `with fs.open` 与 `for line in f` | 不能（`fs` 不是标准库） |
| `07_interactive.py` | `input()` 读标准输入、`while` + `if` 做交互，演示裸名扩展（`input` 来自 `fs.pylib`） | 不能（输入行为由 `fs.pylib` 提供） |

`01` / `02` / `04` 三份能在 CPython 里跑，这件事本身是「AiPy 实现的是
Python 子集」的证据。
`tests/check_examples.py` 每次构建都拿 CPython 的输出逐行比对，跑题了就构建
失败。

**七份都会打包成 exe 实跑一遍**：输出必须与脚本一致。这一步验的是尾部
索引——字节码正确时它仍可能坏——而且 `03` 用了独有内建、`05`/`06`/`07`
用了 pylib，会把「内建名表」和「按需链接」两条路径各走一遍。

`05_threads.py` 不与 CPython 逐行比对（`Thread` 第一个参数是函数名，不是 target 函数），改为断言
四条关键输出。其中 `汇总 TOTAL = 4950` 是关键：四个后台任务各算一段区间，
任何一段没被加进去都会露出来。

## 语言子集的边界

这些语言特性本身都支持（见 `LANGUAGE.md`），示例里不写只是为了让每份
示例**只教一件事**、篇幅短到能一次读完：

- f-string 与 `+` 拼接都能用，示例选了后者
- 元组 `(a, b)` 与列表 `[a, b]` 都是独立类型，示例统一用列表
- 默认参数、关键字传参、`*args`、嵌套函数、`class` 都支持
- 异常可用，但 `parse_number` 这类转换函数按约定失败返回 `None`
- `dict.keys()` 返回列表而不是 `dict_keys`（B6 类差异，`01_basics.py` 里注明了）

`input()` **可以用**（`07_interactive.py` 演示），实现来自 `fs.pylib` 的裸名
注册——与 `open` 同构。一处差异由语言决定：stdin 到底时返回 `None` 而不是
抛 `EOFError`。
