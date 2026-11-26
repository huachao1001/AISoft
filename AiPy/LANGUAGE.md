# AiPy 语言支持清单

**没有支持全部 Python 语法。** 支持的是一份刻意划定的子集：够写脚本、
够打包成 exe，但去掉了所有需要「对象模型、一等函数、模块系统」的特性。

这份清单不是凭记忆写的。每一条都由 `tests/probe_syntax.py` 实测：用
`pcc.exe` 编成字节码、再用 `aipy_run.exe` 真的跑一遍，把结果记下来。
共 196 条，当前 **155 条支持、41 条不支持**。

```bat
:: 重跑一遍（改了编译器后必做）
python tests\probe_syntax.py build\final\pcc.exe build\final\aipy_run.exe
```

> **为什么必须真的跑，不能只看编译**：方法名是运行期查表的。
> `"a".splitlines()` 编译时只是「调用方法，名字叫 splitlines」，
> 要到 VM 里才发现「字符串没有这个方法」。只看编译会把一批不支持的
> 方法误判成支持。

## 一句话概括

| 维度 | 现状 |
|---|---|
| 值类型 | 整数、浮点、字符串（UTF-8）、布尔、None、列表、字典 |
| 变量 | 模块级 + 函数局部（有 `global`） |
| 函数 | 顶层函数、位置参数、递归；**没有**默认值/可变参数/lambda |
| 控制流 | `if`/`elif`/`else`、`while`、`for in`、`break`、`continue` |
| 上下文管理 | `with 管理器:` / `with 管理器 as 名:`；`return`/`break`/`continue` 都会调 `__exit__` |
| 并发/异步 | 无语言层支持（`threading` 是 pylib，不是语言语义） |
| 异常 | `try`/`except`/`else`/`finally`/`raise`，与 CPython 语义一致 |
| 类 | 只有 `class X(Activity)` 这一种形态 |
| 模块 | `import` 只记录名字，实际链接在构建期完成 |

---

## 支持的部分

### 字面量

| 写法 | 说明 |
|---|---|
| `42`、`-42`、`1234567890123` | 整数（i64） |
| `3.14`、`1.5e10` | 浮点（f64） |
| `"text"`、`"""多行"""` | 字符串，UTF-8，按字符索引 |
| `True`、`False`、`None` | |
| `[1, 2, 3]`、`[]`、`[[1], [2]]` | 列表，可嵌套 |
| `{"a": 1}`、`{}` | 字典 |

### 运算符

| 类别 | 支持 |
|---|---|
| 算术 | `+` `-` `*` `/` `//` `%` `**` |
| 位运算 | `&` `\|` `^` `<<` `>>` `~` |
| 一元 | `-x`、`not x` |
| 比较 | `==` `!=` `<` `<=` `>` `>=` `in` `not in` |
| 链式比较 | `1 < x < 10`（中间项只求值一次） |
| 逻辑 | `and`（短路，返回操作数本身）、`or` |
| 三元 | `a if cond else b` |

### 语句

| 写法 | 说明 |
|---|---|
| `x = 1` | 赋值 |
| `a = b = 1` | 链式赋值 |
| `x += 1` 等 | `+=` `-=` `*=` 等增量赋值 |
| `x[0] = 1` | 下标赋值（含切片赋值 `x[1:3] = ...`） |
| `x: int = 1` | 支持；注解本身被忽略（没有 `__annotations__`） |
| `pass` | |
| `global x` | 函数内改模块级变量必须声明 |
| 函数内赋值 | 函数里第一次赋值就是**局部变量**（与 CPython 一致），不写 `global` 不会改到同名的模块级变量 |
| `with m:` | 上下文管理：调 `m.__enter__()`，退出时调 `m.__exit__()` |
| `with m as v:` | 同上，`__enter__` 的返回值绑到 `v` |

`with` 的 `__exit__` **一定会被调用**：正常走完、`return`、循环里 `break` /
`continue`，四条路径都覆盖。这是锁不泄漏的前提，由编译器在每条出口插指令
保证。多条目 `with a, b:` 按嵌套语义展开，退出时反序。

**manager 可以是脚本类**（实现 `__enter__` / `__exit__` 即可），也可以来自
pylib（锁、文件句柄）。`__exit__` 收到 `(exc_type, exc, tb)`，返回真值会
吞掉异常；`exc_type` 是可打印的类对象，`tb` 恒为 `None`（本语言没有
traceback 对象）。`with [1, 2]:` 会报「list 不支持上下文管理器协议」，
与 CPython 一致。

### 异常

| 写法 | 说明 |
|---|---|
| `try:` / `except:` | 裸 `except` 捕获所有 `Exception` 子类 |
| `except ValueError:` | 按类型匹配；`except (A, B):` 元组形式支持 |
| `except ValueError as e:` | `e` 是异常对象，`str(e)` 得到消息 |
| `else:` | 没有异常时执行 |
| `finally:` | 任何路径都执行（含异常外传、`return`） |
| `raise X` / `raise X("msg")` | 抛异常 |
| `raise` | 重抛当前异常（只能在 `except` 里用） |

内建异常类共 25 个，层次与 CPython 一致：`BaseException` →
`Exception` → `ArithmeticError`/`LookupError`/`ValueError`/`TypeError`/
`NameError`/`AttributeError`/`RuntimeError`/`OSError`/`AssertionError` 等。
清单见 `include/aipy/exceptions.h`，编译器与 VM 共用。

`except` 匹配沿继承链：`except ArithmeticError:` 能抓到
`ZeroDivisionError`。

**`finally` 里的 `return` 会吞掉异常**（CPython 语义）：异常路径先跑
`finally`，其中的 `return` 直接结束函数，不再继续抛。

### 控制流

| 写法 | 说明 |
|---|---|
| `if` / `elif` / `else` | 任意层嵌套 |
| `while cond:` | 不支持 `while ... else` |
| `for x in 可迭代:` | 列表、字符串（按字符）、字典（按 key）、`range()` |
| `break` / `continue` | 需要时自动维护 for 的迭代状态 |

### 函数

| 写法 | 说明 |
|---|---|
| `def f():` | 顶层函数 |
| `def f(a, b):` | 位置参数 |
| `return x` / `return` | |
| `def f() -> int:` | 返回类型注解，被忽略 |
| `def f(a: int)` | 参数注解，被忽略 |
| `def f(a: dict[str, int])` | 嵌套泛型注解，同样被忽略 |
| 递归 | 深度上限 256 层，超出报错不崩溃 |
| 前向调用 | 后面的函数可以被前面调用 |
| 文档字符串 | 作为普通字符串表达式，无副作用 |

### 字符串方法

`upper` `lower` `strip` `replace` `split` `startswith` `endswith`
`find` `count` `join` `format`

### 列表方法

`append` `extend` `insert` `pop` `clear` `sort` `reverse`

### 字典方法

`get` `keys` `values` `items` `update` `pop`

> `keys()` / `values()` / `items()` 返回**普通列表**，不是 CPython 的视图对象。

### 下标与切片

| 写法 | 说明 |
|---|---|
| `x[0]`、`x[-1]` | 正/负下标 |
| `x[0:2]`、`x[:2]`、`x[1:]`、`x[::2]` | 切片 |
| `x[::-1]` | 负步长（反向） |
| `d["k"]` | 字典下标 |

### 内建函数（21 个）

| 函数 | 说明 |
|---|---|
| `len` `str` `int` `float` `bool` | 基础 |
| `abs` `round` `min` `max` `sum` | 数值 |
| `list` `dict` `range` `set` | 容器构造（`list`/`sum` 可直接吃生成器） |
| `print` | 输出到 log sink（命令行宿主接 stdout） |
| `format_text(tpl, list)` | 单占位符格式化 |
| `char_from_code` `text_of_char` | 码点与字符互转 |
| `next(生成器)` | 推进一格，耗尽抛 `StopIteration` |
| `type(x)` | 返回 `<class 'int'>` 形式的类型值，脚本类带 `__main__.` 前缀 |
| `isinstance(x, T)` | 认脚本类的继承链与异常层级；`T` 暂不支持元组形式 |

### 类

两种形态。

**一、普通 Python 类**（有真正的对象模型）：

```python
class Animal:
    def __init__(self, name):
        self.name = name
    def speak(self):
        return "..."

class Dog(Animal):
    def speak(self):
        return super().speak() + "!"
```

支持继承、多继承（MRO 深度优先左到右去重）、类属性、`a.b` 读写、
`super().m()`。`self` 是真正的第一个形参。

**二、`class X(Activity)`：脚本入口的简写形式**：

```python
class App(Activity):
    count = 0                    # 字段 → 模块级变量

    def on_start(self):
        self.count = 1           # self.x → x
        self.refresh()           # self.m() → m()

    def refresh(self):
        pass
```

`self` 被去掉，方法展开成顶层函数，字段展开成模块级变量。一个脚本只有
一个 `Activity`。

展开后方法名就是普通函数名，所以 `self.refresh()` 与 `refresh()` 都能调用，
`self.` 会被去掉。字段同理：`self.count` 与 `count` 指的是同一个变量。

**`self.x = ...` 是改字段的正当写法**，不会被「函数里改模块级变量要写
`global`」那道检查拦下来——写 `self.` 的意图就是改实例状态。但直接写
`x = 2`（不带 `self.`）仍然会被拒绝，因为那几乎总是漏写 `global` 的笔误。

这条路径由 `aipy_activity` 测试守着，因为 `Activity` 在 CPython 里没有定义，
没法塞进 `examples/`。

### import

`import x`、`from x import y`、`import x as y` 都能编译——但只**记录名字**，
不加载模块。实际链接由构建期（`pyui_extc`）完成；字节码路线里直接调用
扩展函数会报错并提示用构建期链接表。

---

## 支持面与边界

### 字面量

| 写法 | 状态 |
|---|---|
| 元组 `(1, 2)` | 支持（独立类型 `tuple`） |
| 集合 `{1, 2}` | 支持（独立类型 `set`，按插入序去重） |
| f-string `f"{x}"` | 支持；**不支持**格式说明符与 `!r`/`!s` |
| `bytes` `b"abc"` | 支持 |
| `...` | 不支持，明确报错 |

### 运算符与比较

| 写法 | 状态 |
|---|---|
| `a @ b` 矩阵乘 | 不支持 |
| `:=` 海象 | 不支持，明确报错 |
| `a is b`、`a is not b` | 支持 |
| 切片赋值 `x[1:3] = ...` | 支持 |
| 多维下标 `x[i][j]` | 支持（逐层下标） |

### 语句

| 写法 | 状态 |
|---|---|
| `del x` | 支持 |
| `assert` | 支持 |
| `with a, b:` | 支持多条目，按嵌套语义展开 |
| `match` / `case` | 不支持，用 `if`/`elif` 链 |
| `nonlocal` | 不支持（没有嵌套函数） |
| `type X = int` | 不支持（Python 3.12 语法） |

### 类

| 写法 | 状态 |
|---|---|
| `class A:` | 支持 |
| `class A(Base)`、多继承 | 支持（MRO：深度优先、左到右、去重） |
| `a.b` 读 / `a.b = v` 写 | 支持 |
| `A(...)` 实例化、`__init__` | 支持 |
| `super().m()` | 支持 |
| 类属性 `class A: x = 1` | 支持 |
| `@staticmethod` | 支持：经类与实例访问都不绑接收者 |
| `@classmethod` | 支持：绑**接收者的类**，`super().cm()` 仍是接收者的类 |
| 类成员上的其它装饰器 | 支持，在类体里就地应用（`m = dec(m)`） |
| `class X(Activity)` | 另一种入口形式：类体字段展开成模块级变量，方法展开成顶层函数 |

### 控制流

| 写法 | 替代 |
|---|---|
| `while ... else` | 用标志变量 |
| `for ... else` | 用标志变量 |

### 函数

| 写法 | 状态 |
|---|---|
| 默认参数 `def f(a=1)` | 支持，在**定义处**求值（CPython 语义） |
| 关键字传参 `f(a=1)` | 支持 |
| `*args` / `**kwargs` | 支持 |
| 仅关键字参数 `def f(*, a)` | 支持 |
| 位置-only 参数 `def f(a, /)` | 不支持，明确报错 |
| 嵌套函数 `def` 内套 `def` | 不支持，移到模块顶层 |
| `lambda` | 支持，可作一等值传参 |
| 装饰器 `@dec` | 支持，可多层、可带参 |
| 生成器 `yield` | 支持，真正惰性（调用不执行函数体） |
| `yield from` | 不支持，明确报错 |
| `async def` / `await` | 支持；无事件循环，`await` 直接驱动协程到结束 |
| `async for` / `async with` | 不支持，明确报错 |
| 函数当值 `g = f` | 支持（一等函数） |
| 函数名重新赋值 `f = 3` | 不允许，会静默破坏所有 `f()` 调用 |

### 字符串方法

`splitlines` `title` `zfill` `encode` `lstrip` `rstrip` `center` `ljust`
`rjust` `isdigit` `isalpha` 等——**只有上面列出的 11 个**。

### 列表方法

`index` `remove` `copy` `count` `sort(key=...)` `sorted()`——只有 7 个。
需要排序拷贝就 `x = list(x); x.sort()`。

### 字典方法

`setdefault` `copy` `fromkeys` `clear`——只有 6 个。

### 内建函数

`sorted` `enumerate` `zip` `map` `filter` `any` `all` `reversed`
`help` `dir` `id` `hash` `repr`
`chr` `ord` `bin` `hex` `oct` `divmod` `pow` `sum(生成器)` 全部不支持。

内建类型名（`int` `str` `list` `dict` `ValueError` 等）在**值位置**是可用的：
`type(x) is int`、`isinstance(x, str)`、`f(int)` 都成立。调用位置仍然是内建
函数——`int("3")` 走 `CALL_BUILTIN`，两者由语法位置区分，不需要额外标记。

`open` 与 `input` **支持**，但它们的实现来自 `fs.pylib`：库用
`AIPY_PYLIB_BUILTIN` 把它们注册成裸名，所以脚本里写 `open(p)` / `input()`
不用 import。这是「库能注册内建」的机制，不是编译器内置了它们。

`input` 的语义对齐 CPython（提示语不换行、返回值剥掉行尾、`\r\n` 也剥），
一处差异由语言决定：**stdin 到底时返回 `None` 而不是抛 `EOFError`**——
`aipy_input` 拿 CPython 逐字节对拍守着这几点。

上表那些名字现在**编译期不再报错**（它们被编成按名字的扩展调用），而是在
**打包期**报「没有任何库声明这个名字」。原因：编译器不认识具体能力，而库
是可插拔的——同一个脚本在不同产物里可以链不同的库。代价是拼写错误推迟到
打包期，换来的是新增裸名不用改编译器。

内建也不能当值用：`f = len` 报错。

### 下标与切片

| 写法 | 状态 |
|---|---|
| 切片赋值 `x[0:1] = [...]` | 支持 |
| 多维下标 `x[0][1]` | 支持（逐层下标） |

### 模块与类

| 写法 | 说明 |
|---|---|
| 普通 `class A:` | 支持，有真正的对象模型 |
| `class A(Base)`、多继承 | 支持（MRO 深度优先、左到右、去重） |
| 属性访问 `a.b` | 支持，读写皆可 |
| `super()` | 支持 |
| `import` 后实际使用 | 需要构建期链接表 |

---

## 与 CPython 的语义差异

即使语法支持，行为也可能不同。这些是**刻意**的差异，不是 bug：

| 场景 | CPython | AiPy |
|---|---|---|
| `d.keys()` | `dict_keys([...])` 视图 | 普通列表 `[...]` |
| 函数传参 | 引用传递，支持关键字 | 按位置，参数个数固定 |
| 错误处理 | traceback | `file:line: 原因`，带源行号 |
| 递归深度 | 约 1000 层 | 256 层 |

### 必须记住的一处静默差异

```python
# 整数溢出静默回绕（64 位有符号）
x = 9223372036854775807
x = x + 1          # 变成 -9223372036854775808，不报错
```

整数溢出属于「64 位实现」的必然结果（`Value` 只有 16 字节，放不下大整数），
涉及金额、时间戳时留意范围。

异常的语义与 CPython 一致：`1 / 0` 抛 `ZeroDivisionError`，
`int("abc")` 抛 `ValueError`，`open` 失败抛 `FileNotFoundError` 等。

---

## 快速判断：能不能写

写下你想用的写法，对照这三条：

1. **需要「值」是一段代码吗？**（函数当参数、lambda、装饰器）→ 不支持
2. **需要对象吗？**（`class`、属性、`self.x` 之外）→ 只有 `Activity`

两条都否，多半能写。再查上面的方法表确认具体方法名。

## 维护

改了编译器或 VM 之后：

```bat
python tests\probe_syntax.py build\final\aipy_run.exe
```

输出末尾会列出「与预期不符」的条目。`tests/probe_syntax.py` 里每条用例都
标了期望值（`ok` / `no`）；文档说支持的必须真的能跑，说不支持的必须真的
报错。**这条检查没有进 CTest**——它跑 184 次编译+执行，约 10 秒，
放在每次构建里太重；提交前手动跑一次即可。
