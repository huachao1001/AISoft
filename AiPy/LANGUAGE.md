# AiPy 语言支持清单

**没有支持全部 Python 语法。** 支持的是一份刻意划定的子集：够写脚本、
够打包成 exe，但去掉了所有需要「对象模型、异常通道、一等函数、模块系统」
的特性。

这份清单不是凭记忆写的。每一条都由 `tests/probe_syntax.py` 实测：用
`pcc.exe` 编成字节码、再用 `aipy_run.exe` 真的跑一遍，把结果记下来。
共 186 条，当前 **146 条支持、40 条不支持**。

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
| 异常 | 无（失败用返回值表示，如 `parse_number` 返回 `None`） |
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
| `x[0] = 1` | 下标赋值（单下标，不支持切片赋值） |
| `x: int = 1` | 类型注解，注解本身被忽略 |
| `pass` | |
| `global x` | 函数内改模块级变量必须声明 |
| 函数内赋值 | 函数里第一次赋值就是**局部变量**（与 CPython 一致），不写 `global` 不会改到同名的模块级变量 |
| `with m:` | 上下文管理：调 `m.__enter__()`，退出时调 `m.__exit__()` |
| `with m as v:` | 同上，`__enter__` 的返回值绑到 `v` |

`with` 的 `__exit__` **一定会被调用**：正常走完、`return`、循环里 `break` /
`continue`，四条路径都覆盖。这是锁不泄漏的前提，由编译器在每条出口插指令
保证。一次只支持一个 manager；`with a, b:` 要嵌套写。

**manager 必须来自 pylib**：语言里没有可实例化的类（只有 `class X(Activity)`），
所以插件的 `__enter__`/`__exit__` 只能由 `kObject`（带方法表的宿主对象）提供。
`with [1, 2]:` 会报「列表没有方法 `__enter__`」，这是预期行为。

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

### 内建函数（18 个）

| 函数 | 说明 |
|---|---|
| `len` `str` `int` `float` `bool` | 基础 |
| `abs` `round` `min` `max` `sum` | 数值 |
| `list` `dict` `range` | 容器构造 |
| `print` | 输出到 log sink（命令行宿主接 stdout） |
| `format_text(tpl, list)` | 单占位符格式化，替代 f-string |
| `parse_number(text)` | 文本转数字，失败返回 `None` |
| `char_from_code` `text_of_char` | 码点与字符互转 |

### 类

只有一种形态：

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

这条路径由 `aipy_activity` 测试守着（13 条：7 条实跑比对输出、6 条期望被
拒绝），因为 `Activity` 在 CPython 里没有定义，没法塞进 `examples/`。

### import

`import x`、`from x import y`、`import x as y` 都能编译——但只**记录名字**，
不加载模块。实际链接由构建期（`pyui_extc`）完成；字节码路线里直接调用
扩展函数会报错并提示用构建期链接表。

---

## 不支持的部分

### 值与字面量

| 写法 | 为什么不支持 |
|---|---|
| 元组 `(1, 2)` | 容器只有列表和字典（沿用 AOT 路线 B6：元组当列表） |
| 集合 `{1, 2}` | 没有 `set` |
| f-string `f"{x}"` | 用 `format_text` 或 `+` 拼 |
| `bytes` `b"abc"` | 没有字节串类型 |
| `...` | 没有用途 |

### 运算符与比较

| 写法 | 替代 |
|---|---|
| `a @ b` 矩阵乘 | 无 |
| `:=` 海象 | 先赋值再用 |
| `a is b`、`a is not b` | 用 `==`；判 `None` 也写 `== None` |

### 语句

| 写法 | 替代 |
|---|---|
| `del x` | 容器用 `.pop()` / `.clear()` |
| `assert` | `if` + 显式处理 |
| `raise` / `try` / `except` / `finally` | 没有异常通道，用返回值表示失败 |
| `with a, b:` | 一次只支持一个 manager；嵌套写多个 `with` |
| `match` / `case` | 用 `if`/`elif` 链 |
| `nonlocal` | 没有嵌套函数 |
| `type X = int` | Python 3.12 语法，不支持 |

### 控制流

| 写法 | 替代 |
|---|---|
| `while ... else` | 用标志变量 |
| `for ... else` | 用标志变量 |

### 函数

| 写法 | 替代 |
|---|---|
| 默认参数 `def f(a=1)` | 在函数体内判断，或调用处显式传 |
| 关键字传参 `f(a=1)` | 一律按位置传 |
| `*args` / `**kwargs` | 参数个数固定 |
| 仅关键字参数 `def f(*, a)` | 无 |
| 位置-only 参数 `def f(a, /)` | 无（本来就没有关键字传参可区分） |
| 嵌套函数 | 移到模块顶层 |
| `lambda` | 定义具名函数 |
| 装饰器 `@dec` | 无 |
| 生成器 `yield` | 返回列表 |
| `async` / `await` | 单线程，无事件循环 |
| 函数当值 `g = f` | 没有一等函数，直接调用 |
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
`isinstance` `type` `help` `dir` `id` `hash` `repr`
`chr` `ord` `bin` `hex` `oct` `divmod` `pow` `sum(生成器)` 全部不支持。

`open` 与 `input` **支持**，但它们的实现来自 `fs.pylib`：库用
`AIPY_PYLIB_BUILTIN` 把它们注册成裸名，所以脚本里写 `open(p)` / `input()`
不用 import。这是「库能注册内建」的机制，不是编译器内置了它们。

`input` 的语义对齐 CPython（提示语不换行、返回值剥掉行尾、`\r\n` 也剥），
一处差异由语言决定：**stdin 到底时返回 `None` 而不是抛 `EOFError`**——
这个语言没有异常通道，失败一律靠返回值。`aipy_input` 拿 CPython 逐字节对拍
守着这几点。

上表那些名字现在**编译期不再报错**（它们被编成按名字的扩展调用），而是在
**打包期**报「没有任何库声明这个名字」。原因：编译器不认识具体能力，而库
是可插拔的——同一个脚本在不同产物里可以链不同的库。代价是拼写错误推迟到
打包期，换来的是新增裸名不用改编译器。

内建也不能当值用：`f = len` 报错。

### 下标与切片

| 写法 | 替代 |
|---|---|
| 切片赋值 `x[0:1] = [...]` | 逐个下标赋值，或用 `append`/`pop` |
| 多维下标 `x[0][1]` | 这个可以：`x[0]` 先取列表再取下标 |

### 模块与类

| 写法 | 说明 |
|---|---|
| 普通 `class A:` | 类必须继承 `Activity` |
| `class A(Base)` | 只认 `Activity` |
| 属性访问 `a.b` | 没有对象模型；只有 `self.x`（展开成模块级变量） |
| 多继承、`super()` | 无对象模型 |
| `import` 后实际使用 | 需要构建期链接表 |

---

## 与 CPython 的语义差异

即使语法支持，行为也可能不同。这些是**刻意**的差异，不是 bug：

| 场景 | CPython | AiPy |
|---|---|---|
| `d.keys()` | `dict_keys([...])` 视图 | 普通列表 `[...]` |
| 函数传参 | 引用传递，支持关键字 | 按位置，参数个数固定 |
| 异常 | 抛异常 | 返回 `None` 或报错停机 |
| **除以零** | 抛 `ZeroDivisionError` | **静默返回 `None`** |
| **整数溢出** | 自动转大整数 | **静默回绕到 i64** |
| 错误处理 | traceback | `file:line: 原因`，带源行号 |
| 递归深度 | 约 1000 层 | 256 层 |

### 两条必须记住的静默差异

```python
# 1. 除以零不抛异常，返回 None
x = 5 / 0          # x 是 None，脚本继续跑
print(x)           # 输出 None

# 2. 整数溢出静默回绕（64 位有符号）
x = 9223372036854775807
x = x + 1          # 变成 -9223372036854775808，不报错
```

**为什么要你记住**：没有异常通道，除零只能靠返回值表达失败。所以脚本里凡
是可能除零的地方，都要自己判：

```python
if count > 0:
    avg = total / count
else:
    avg = 0
```

判 `None` 也可以（`if avg == None:`），但**先判除数更可靠**——`None` 一旦
传下游，下一次运算只会再得到 `None`，错误要很久以后才显形。

整数溢出属于「64 位实现」的必然结果（`Value` 只有 16 字节，放不下大整数），
涉及金额、时间戳时留意范围。

---

## 快速判断：能不能写

写下你想用的写法，对照这三条：

1. **需要异常吗？** 需要 → 用返回值表示失败
2. **需要「值」是一段代码吗？**（函数当参数、lambda、装饰器）→ 不支持
3. **需要对象吗？**（`class`、属性、`self.x` 之外）→ 只有 `Activity`

三条都否，多半能写。再查上面的方法表确认具体方法名。

## 维护

改了编译器或 VM 之后：

```bat
python tests\probe_syntax.py build\final\aipy_run.exe
```

输出末尾会列出「与预期不符」的条目。`tests/probe_syntax.py` 里每条用例都
标了期望值（`ok` / `no`）；文档说支持的必须真的能跑，说不支持的必须真的
报错。**这条检查没有进 CTest**——它跑 184 次编译+执行，约 10 秒，
放在每次构建里太重；提交前手动跑一次即可。
