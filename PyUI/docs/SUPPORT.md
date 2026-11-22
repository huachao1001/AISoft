# Python 子集支持矩阵

> 结论先写：**语法覆盖等于 CPython**（前端就是 `ast`），**语义覆盖是本表**。
> 表里没写的写法，构建期会报错，不会静默算错。

`pyuic` 在构建期把 Python 脚本编译成 C++，运行期没有解释器。因此有两条
边界必须显式化：

- **语法**：能解析多少？——全部。解析器就是当前 CPython 自己的。
- **语义**：解析之后能翻译多少？——见下表的 A / B / C。

这张表的每一条都有对应的自动化守护：

| 守护 | 覆盖什么 |
|---|---|
| `tools/pyuic/check_syntax.py`（CTest: `pyuic_syntax`） | 53 个语法用例，逐条断言「能生成」还是「必须报错」 |
| `tools/pyuic/check_scope.py`（CTest: `pyuic_scope`） | 函数局部量集合与 CPython 的 `symtable` 逐字对拍 |
| `tools/pyuic/check_value_semantics.py`（CTest: `pyuic_value_semantics`） | 运行时语义的期望值拿 CPython 当基准 |
| `tests/test_py_value.cpp` | 上述语义在 C++ 侧的实际行为 |

---

## A 类：能机械翻译，结果与 CPython 一致

这些写法**逐条有测试**，可以放心用。

### 语句

| 写法 | 备注 |
|---|---|
| `x = 1` / `a = b = 1` | 模块级是成员变量，函数内是局部量 |
| `x += 1` / `d[k] += 1` | 下标目标按「读-改-写」生成 |
| `x: int = 1` | 注解在运行期没有意义，被丢弃 |
| `if / elif / else` | |
| `while` + `break` / `continue` | |
| `for x in range(a, b, step)` | 直接编成 C++ 的 `for` |
| `for x in <列表/字符串/字典>` | 循环变量在循环结束后仍然存在，与 CPython 一致 |
| `return` | |
| `global x` | **必须写**，理由见 C 类 |

### 表达式

| 写法 | 备注 |
|---|---|
| 算术 `+ - * / // % **` | 整数真除法、向下取整除法、Python 符号的取模都已对齐 |
| 位运算 `& \| ^ << >> ~` | |
| 比较 `== != < <= > >=` | 数值跨类型按值比（`1 == 1.0` 为真） |
| 链式比较 `a < b < c` | 中间项只求值一次 |
| 逻辑 `and` / `or` / `not` | **返回值是操作数本身**，且短路求值 |
| 条件表达式 `x if c else y` | 只求值一个分支 |
| `in` / `not in` | 列表按值、字符串按子串 |
| 字符串字面量、f-string（无格式说明） | `f"{x}"`、`f"{x!s}"` |
| 列表/字典字面量 | 字典**保留插入顺序** |
| 下标 `x[i]`、切片 `x[a:b]` | 负数下标从尾部算；字符串按**字符**（不是字节） |
| 一元 `-` / `+` | |

### 内建函数

`int` `float` `str` `bool` `len` `abs` `round` `min` `max` `sum` `list`
`dict` `range` `print`，以及 pyui 提供的 `read_file` `write_file`
`join_path` `home_directory` `format_text` `parse_number` `char_from_code`。

### 方法

字符串：`upper` `lower` `strip` `replace` `split` `join` `startswith`
`endswith` `find` `count` `format`。

列表：`append` `extend` `insert` `pop` `clear` `sort` `reverse`。

字典：`get` `keys` `values` `items` `update`。

### 函数

| 写法 | 备注 |
|---|---|
| `def` / 调用 / 参数 | 参数按值传 `py::Value` |
| 默认参数 `def f(a, b=2)` | **只支持字面量**，理由见 B 类 |
| 仅位置参数 `def f(a, /, b)` | `/` 之后的部分同样按位置处理 |
| 多返回值（返回列表） | `return a, b` 返回的是列表，`a, b = f()` 不支持（见 C 类） |

---

## B 类：能翻译，但存在已知差异

每一条都写清了差异的**方向**和**为什么**。用之前先确认能接受。

### B1. `str(float)` —— 已对齐，但要知道它长什么样

`str(0.1 + 0.2)` 是 `"0.30000000000000004"`，不是 `"0.3"`。这不是缺陷，
是 CPython 的行为，产物与之一致。

```python
str(0.1 + 0.2)   # "0.30000000000000004"
str(1 / 3)       # "0.3333333333333333"
str(1e15)        # "1000000000000000.0"
str(1e16)        # "1e+16"
```

**做显示时不要用 `str()`**，用 `round()` 或 `format_text`：

```python
set_text("result", str(round(total, 2)))   # "0.3"
```

### B2. 浮点运算误差 —— 与 CPython 完全相同，不需要处理

`0.1 + 0.2 == 0.3` 为假，这是 IEEE754 的行为，两边一致。**不要试图修正**，
一改就与解释器不一致了。

### B3. 默认参数只在构建期展开一次

```python
def f(a, b=[])      # ❌ 报错
def f(a, b=1)       # ✅
```

CPython 的默认值在 `def` 执行时求值一次，可变默认值会跨调用共享。生成代码
把默认值放在调用点的默认实参里，语义不同。所以**只接受字面量**，非字面量
构建期报错，不给出看起来对、实际时机不同的代码。

### B4. 整数是 64 位，这是最明显的一处差异

CPython 的 `int` 任意精度，产物用 `long long`（±9.2×10^18）。所以：

| 写法 | CPython | 产物 |
|---|---|---|
| `2 ** 100` | `1267650600228229401496703205376` | 溢出（未定义） |
| `10 ** 30` | 精确 | 溢出 |
| 大数乘法 | 精确 | 溢出 |

`2 ** 10` 这类结果在范围内的运算**类型正确**（整数幂得到 `int`，不是
`float`），只有溢出时不可靠。

**规避**：超过约 9.2×10^18 的数改用 `float`。界面应用里极少需要。

### B5. 字符串按字符索引，但按字节比较

```python
"中文"[1]        # "文"   ✅ 按字符
"中文" < "中文a"  # 与 CPython 一致（UTF-8 字节序与码点序同序）
```

UTF-8 的字节序恰好与 Unicode 码点序一致，所以字符串比较不需要特殊处理。
唯一的差异在**不合法 UTF-8**（产物不会产生，忽略）。

### B6. 容器没有 `dict` 之外的变体

`set`、`frozenset`、`tuple` 都没有。元组字面量 `(1, 2)` 会被当成列表。
所以 `a, b = f()` 也不支持（见 C 类）。

### B7. 取模/整除的对齐

```python
-7 // 2   # -4
-7 % 2    # 1
7.5 // 2  # 3.0
```

C++ 的 `/` 向零截断、`%` 结果符号跟被除数；产物已按 Python 规则改写，
这三条都有测试。

---

## C 类：构建期报错（不会静默算错）

以下写法**一律**在构建期报错并给出 `file:line:col`。它们的共同点是：
生成代码无法保持 CPython 语义，静默翻译就会得到「编译通过、算出来错」的
结果——那比报错糟糕得多。

### 没有的东西

| 写法 | 报错文案（要点） |
|---|---|
| `import` | 没有模块系统；需要的能力用 pyui 的函数 |
| `class` | 没有对象模型；脚本状态用模块级变量 |
| 嵌套函数 / `nonlocal` | 没有闭包；移到模块顶层 |
| `lambda` | 没有一等函数 |
| 装饰器 | 装饰器会替换函数对象，无从生效 |
| `try` / `except` / `finally` | 没有异常通道；失败用返回值表示 |
| `raise` | 同上 |
| `assert` | 用 `if` + 显式处理 |
| `with` | 没有上下文管理器 |
| `yield` / `yield from` | 没有生成器（`yield` 曾被静默丢弃，现已在构建期拦下） |
| `async def` / `await` | 单线程事件循环 |
| `match` / `case` | 未实现 |

### 没有的数据类型

| 写法 | 报错文案（要点） |
|---|---|
| 集合字面量 `{1, 2}` | 用列表 |
| `bytes` 字面量 | 用字符串 |
| `...`（Ellipsis） | |
| 矩阵乘法 `@` | |

### 语法层面的限制

| 写法 | 报错文案（要点） |
|---|---|
| 列表/字典/生成器推导 | 用 `for` 循环 + `.append()` |
| 海象运算符 `:=` | |
| 元组解包 `a, b = ...` | 分开写两行 |
| 切片赋值 `x[a:b] = ...` | 只支持单下标赋值 |
| `*展开`（`f(*args)`、`[*a]`） | 参数个数必须固定 |
| `*args` / `**kwargs` / 仅关键字参数 | 参数个数必须固定 |
| 关键字传参 `f(a=1)` | 按位置传 |
| `while ... else` / `for ... else` | |
| `del` | 容器用 `.pop()` / `.clear()` |
| f-string 格式说明 `f"{x:>4}"` | 需要迷你语言；用 `format_text` |
| f-string `!r` / `!a` | 需要 `repr` 语义 |

### 最容易被忽略的一条：`global`

```python
count = 0

def on_click():
    count = count + 1    # ❌ 构建期报错
```

CPython 里这行**不会**改模块级的 `count`——它新建了一个同名局部变量，
`count + 1` 读的正是这个尚未赋值的局部变量（运行期抛 `UnboundLocalError`）。
没有 `global` 时生成代码必须照 CPython 做，但那样几乎总是笔误，所以
**构建期直接报错**：

```
calc.py:5:5: error: 函数里给模块级变量 count 赋值，但没有写 global 声明。
CPython 会把它当成新的局部变量，模块级的值不会变；要改模块级变量请先写 `global count`
```

正确写法：

```python
def on_click():
    global count
    count = count + 1
```

这条规则由 `check_scope.py` 守着——它用 CPython 的 `symtable` 算一遍局部量
集合，与生成器算的比对，任何偏差都会让 CTest 失败。

---

## 两处「看起来像 A 类，其实是 C 类」的坑

这两个是实测中发现的，写下原因以免将来有人「优化」回去。

### 1. `yield` 曾经被静默丢弃

语句生成器里「纯表达式直接丢弃」那条捷径会把 `yield` 也吞掉：

```python
def f():
    yield 1       # 旧行为：整句消失，f 变成空函数
```

现在在 `ast.Expr` 分支里先拦 `Yield` / `YieldFrom` / `Await`，再判断是否
丢弃。凡是「丢弃一个节点」的地方，都要先确认它没有语义。

### 2. 装饰器曾经被静默忽略

`collect()` 只看 `node.name`，`decorator_list` 从未被读。所以
`@staticmethod` 编译通过、行为消失。现在只要 `decorator_list` 非空就报错。

---

## 如何自己验证

```bat
rem 语法覆盖
D:\Software\miniconda3\envs\ys\python.exe tools\pyuic\check_syntax.py

rem 作用域与 CPython 对拍
D:\Software\miniconda3\envs\ys\python.exe tools\pyuic\check_scope.py examples\calculator\calculator.py

rem 运行时语义与 CPython 对拍
D:\Software\miniconda3\envs\ys\python.exe tools\pyuic\check_value_semantics.py

rem 三者都已接入 CTest
build\build_cmake.bat
```

加新语法时的顺序是：先在 `check_syntax.py` 的 `CASES` 里写一条期望
（`ok` 还是 `error`），再去改生成器让测试变绿。先写测试的好处是「它到底
属于 A 类还是 C 类」必须当场想清楚，而不是事后补文档。
