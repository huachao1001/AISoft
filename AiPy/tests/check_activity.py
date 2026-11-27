#!/usr/bin/env python3
"""`class X(Activity)` 这条展开路径的端到端验证。

── 为什么要单独一份 ──

`Activity` 是 AiPy 唯一的类形态：方法展开成顶层函数，`self.x` 展开成模块级
变量，`self.m()` 展开成 `m()`。这条路径有三处容易错，而且都不是编译能看出来的：

1. `self.x = 1` 展开成 `x = 1` 后，会撞上「函数里改模块级变量要写 global」
   那道检查。写 `self.x` 的意图本来就是改实例状态，不该被拦。
2. 方法里调用别的方法（`self.g()`），要在所有方法都登记之后才解析得通。
3. `self.n += 1` 走的是增强赋值，展开后仍然必须是「读-改-写」同一份状态。

这些错误不会崩溃，只会算错或者报一句莫名其妙的 global 提示。所以每条
都真的跑一遍，比对输出。

── 为什么不能放进 check_examples.py ──

`Activity` 在 CPython 里没有定义，示例必须两边都能跑（那是「实现的是
Python 子集」的证据）。这条路径只能在 AiPy 里验，期望值只能手写——
手写有风险，所以用例刻意写得短，每条的期望值一眼能核对。

用法：check_activity.py <pcc.exe> <aipy_run.exe>
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))


def compile_script(pcc, source, scratch, name):
    """用发布件编译器编一份脚本，返回 (字节码路径, 错误文本 或 None)。"""
    script = os.path.join(scratch, name + '.py')
    with open(script, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(source)
    path = os.path.join(scratch, name + '.bc')
    if os.path.isfile(path):
        os.remove(path)
    completed = subprocess.run([pcc, script, path], capture_output=True,
                               text=True, encoding='utf-8', errors='replace')
    if completed.returncode != 0:
        return (None, completed.stderr.strip())
    return (path, None)


def run(pcc, aipy_run, source, scratch, name):
    path, error = compile_script(pcc, source, scratch, name)
    if error is not None:
        raise SystemExit('%s 编译失败：%s' % (name, error))
    completed = subprocess.run([aipy_run, path], capture_output=True, text=True,
                               encoding='utf-8')
    if completed.returncode != 0:
        raise SystemExit('%s 执行失败：%s' % (name, completed.stderr.strip()))
    return completed.stdout


def reject(pcc, source, scratch, name):
    """期望被编译期拒绝，返回错误文本。"""
    path, error = compile_script(pcc, source, scratch, name)
    if error is not None:
        return error
    raise SystemExit('%s 本该被拒绝，却编译通过了' % name)


# (用例名, 源码, 期望 stdout 或 None 表示期望被拒绝)
CASES = [
    (
        'self.x 赋值后读回',
        'class App(Activity):\n'
        '    x = 0\n'
        '    def f(self):\n'
        '        self.x = 5\n'
        '        print(self.x)\n'
        'f()\n',
        '5\n',
    ),
    (
        'self.n += 1 增强赋值',
        'class App(Activity):\n'
        '    n = 0\n'
        '    def tick(self):\n'
        '        self.n += 1\n'
        '    def go(self):\n'
        '        self.tick()\n'
        '        self.tick()\n'
        '        self.tick()\n'
        '        print(self.n)\n'
        'go()\n',
        '3\n',
    ),
    (
        '方法互相调用（前向）',
        'class App(Activity):\n'
        '    def a(self):\n'
        '        print(self.b())\n'
        '    def b(self):\n'
        '        return 42\n'
        'a()\n',
        '42\n',
    ),
    (
        '类体字段作为初值',
        'class App(Activity):\n'
        '    name = "起点"\n'
        '    def show(self):\n'
        '        print(self.name)\n'
        'show()\n',
        '起点\n',
    ),
    (
        '字段是列表，原地修改',
        'class App(Activity):\n'
        '    items = []\n'
        '    def add(self, v):\n'
        '        self.items.append(v)\n'
        '    def go(self):\n'
        '        self.add(1)\n'
        '        self.add(2)\n'
        '        print(self.items)\n'
        'go()\n',
        '[1, 2]\n',
    ),
    (
        '方法带参数',
        'class App(Activity):\n'
        '    base = 10\n'
        '    def add(self, n):\n'
        '        return self.base + n\n'
        'print(add(5))\n',
        '15\n',
    ),
    (
        '方法里改字段再条件判断',
        'class App(Activity):\n'
        '    total = 0\n'
        '    def acc(self, v):\n'
        '        self.total += v\n'
        '        if self.total > 5:\n'
        '            self.total = 0\n'
        '    def go(self):\n'
        '        self.acc(4)\n'
        '        self.acc(4)\n'
        '        print(self.total)\n'
        'go()\n',
        '0\n',
    ),
    # ── 普通 Python 类（真正的对象模型）──
    (
        '普通类实例字段',
        'class P:\n'
        '    def __init__(self, x):\n'
        '        self.x = x\n'
        'p = P(7)\n'
        'print(p.x)\n',
        '7\n',
    ),
    (
        '普通类方法调用',
        'class P:\n'
        '    def __init__(self, x):\n'
        '        self.x = x\n'
        '    def double(self):\n'
        '        return self.x * 2\n'
        'print(P(4).double())\n',
        '8\n',
    ),
    (
        '类继承与方法覆盖',
        'class A:\n'
        '    def who(self):\n'
        '        return "A"\n'
        'class B(A):\n'
        '    def who(self):\n'
        '        return "B"\n'
        'print(A().who())\n'
        'print(B().who())\n',
        'A\nB\n',
    ),
    (
        'super() 调父类',
        'class A:\n'
        '    def who(self):\n'
        '        return "A"\n'
        'class B(A):\n'
        '    def who(self):\n'
        '        return super().who() + "B"\n'
        'print(B().who())\n',
        'AB\n',
    ),
    (
        '多继承按基类顺序',
        'class A:\n'
        '    def who(self):\n'
        '        return "A"\n'
        'class B:\n'
        '    def who(self):\n'
        '        return "B"\n'
        'class C(A, B):\n'
        '    pass\n'
        'print(C().who())\n',
        'A\n',
    ),
    # ── 生成器 ──
    (
        '生成器逐个产出',
        'def nums(n):\n'
        '    i = 0\n'
        '    while i < n:\n'
        '        yield i * i\n'
        '        i = i + 1\n'
        'for v in nums(4):\n'
        '    print(v)\n',
        '0\n1\n4\n9\n',
    ),
    (
        '生成器惰性（创建不执行）',
        'def g():\n'
        '    print("start")\n'
        '    yield 1\n'
        'print("created")\n'
        'for v in g():\n'
        '    print(v)\n',
        'created\nstart\n1\n',
    ),
    (
        '生成器嵌套 for',
        'def pairs():\n'
        '    for x in [1, 2]:\n'
        '        for y in ["a", "bb"]:\n'
        '            yield x * 100 + len(y)\n'
        'for v in pairs():\n'
        '    print(v)\n',
        '101\n102\n201\n202\n',
    ),
    (
        '生成器 break 后不再继续',
        'def g():\n'
        '    yield 1\n'
        '    yield 2\n'
        'for v in g():\n'
        '    print(v)\n'
        '    break\n'
        'print("done")\n',
        '1\ndone\n',
    ),
    (
        '生成器异常向外传播',
        'def g():\n'
        '    yield 1\n'
        '    raise ValueError("x")\n'
        'try:\n'
        '    for v in g():\n'
        '        print(v)\n'
        'except ValueError:\n'
        '    print("caught")\n',
        '1\ncaught\n',
    ),
    (
        'next() 逐个取值',
        'def g():\n'
        '    yield "a"\n'
        '    yield "b"\n'
        'it = g()\n'
        'print(next(it))\n'
        'print(next(it))\n',
        'a\nb\n',
    ),
    (
        'list() 抽干生成器',
        'def g():\n'
        '    yield 1\n'
        '    yield 2\n'
        'print(list(g()))\n',
        '[1, 2]\n',
    ),
    # ── async / await ──
    (
        'async 协程依次执行',
        'async def step(x):\n'
        '    print("step")\n'
        '    return x * 2\n'
        'async def main():\n'
        '    a = await step(1)\n'
        '    b = await step(2)\n'
        '    print(a + b)\n'
        'main()\n'
        'await main()\n',
        'step\nstep\n6\n',
    ),
    (
        'async 惰性（创建不执行）',
        'async def c():\n'
        '    print("body")\n'
        '    return 1\n'
        'x = c()\n'
        'print("created")\n'
        'print(await x)\n',
        'created\nbody\n1\n',
    ),
    (
        'async 里抛异常向外传播',
        'async def bad():\n'
        '    raise ValueError("x")\n'
        'async def main():\n'
        '    try:\n'
        '        await bad()\n'
        '    except ValueError:\n'
        '        print("caught")\n'
        'await main()\n',
        'caught\n',
    ),
    # ── staticmethod / classmethod ──
    (
        'staticmethod 不绑 self',
        'class A:\n'
        '    @staticmethod\n'
        '    def add(x, y):\n'
        '        return x + y\n'
        'print(A.add(1, 2))\n'
        'print(A().add(3, 4))\n',
        '3\n7\n',
    ),
    (
        'classmethod 绑接收者的类',
        'class Base:\n'
        '    @classmethod\n'
        '    def who(cls):\n'
        '        if cls is Base:\n'
        '            return "Base"\n'
        '        return "sub"\n'
        'class Derived(Base):\n'
        '    pass\n'
        'print(Base.who())\n'
        'print(Derived.who())\n'
        'print(Derived().who())\n',
        'Base\nsub\nsub\n',
    ),
    (
        'classmethod 里 super() 仍绑接收者的类',
        'class Base:\n'
        '    @classmethod\n'
        '    def who(cls):\n'
        '        if cls is Base:\n'
        '            return "Base"\n'
        '        return "sub"\n'
        'class Derived(Base):\n'
        '    @classmethod\n'
        '    def who(cls):\n'
        '        return "D[" + super().who() + "]"\n'
        'class Leaf(Derived):\n'
        '    pass\n'
        'print(Leaf.who())\n'
        'print(Derived.who())\n',
        'D[sub]\nD[sub]\n',
    ),
    (
        'staticmethod 经继承与实例访问',
        'class Base:\n'
        '    @staticmethod\n'
        '    def tag():\n'
        '        return "static"\n'
        'class Derived(Base):\n'
        '    pass\n'
        'print(Derived.tag())\n'
        'print(Derived().tag())\n'
        'print(Base.tag())\n',
        'static\nstatic\nstatic\n',
    ),
    (
        '类成员的普通装饰器生效',
        'def replace(f):\n'
        '    return other\n'
        'def other(obj):\n'
        '    return 999\n'
        'class A:\n'
        '    @replace\n'
        '    def m(self):\n'
        '        return 1\n'
        'print(A().m())\n',
        '999\n',
    ),
    (
        '两个不同的类用 is 比是 False',
        'class A:\n'
        '    pass\n'
        'class B(A):\n'
        '    pass\n'
        'print(B is A)\n'
        'print(A is A)\n',
        'False\nTrue\n',
    ),
    # ── with 上下文协议（脚本类）──
    (
        '脚本类的 with 进入与退出',
        'class Guard:\n'
        '    def __init__(self, name):\n'
        '        self.name = name\n'
        '    def __enter__(self):\n'
        '        print("enter " + self.name)\n'
        '        return self.name\n'
        '    def __exit__(self, t, e, tb):\n'
        '        print("exit " + self.name)\n'
        'with Guard("a"):\n'
        '    print("body")\n'
        'print("done")\n',
        'enter a\nbody\nexit a\ndone\n',
    ),
    (
        '多条目 with 按嵌套语义退出',
        'class Guard:\n'
        '    def __init__(self, n):\n'
        '        self.n = n\n'
        '    def __enter__(self):\n'
        '        print("enter " + self.n)\n'
        '        return self.n\n'
        '    def __exit__(self, t, e, tb):\n'
        '        print("exit " + self.n)\n'
        'with Guard("a"), Guard("b"):\n'
        '    print("body")\n',
        'enter a\nenter b\nbody\nexit b\nexit a\n',
    ),
    (
        'with as 拿到 __enter__ 的返回值',
        'class Guard:\n'
        '    def __enter__(self):\n'
        '        return 42\n'
        '    def __exit__(self, t, e, tb):\n'
        '        pass\n'
        'with Guard() as v:\n'
        '    print(v)\n',
        '42\n',
    ),
    (
        '__exit__ 返回真值吞掉异常',
        'class Suppress:\n'
        '    def __enter__(self):\n'
        '        return self\n'
        '    def __exit__(self, t, e, tb):\n'
        '        print(t)\n'
        '        return True\n'
        'with Suppress():\n'
        '    raise ValueError("boom")\n'
        'print("after")\n',
        "<class 'ValueError'>\nafter\n",
    ),
    (
        '__exit__ 返回假值异常继续传播',
        'class NoSuppress:\n'
        '    def __enter__(self):\n'
        '        return self\n'
        '    def __exit__(self, t, e, tb):\n'
        '        print("saw")\n'
        '        return False\n'
        'try:\n'
        '    with NoSuppress():\n'
        '        raise ValueError("boom")\n'
        'except ValueError:\n'
        '    print("caught")\n',
        'saw\ncaught\n',
    ),
    (
        'with 体内 return 也会退出',
        'class Guard:\n'
        '    def __enter__(self):\n'
        '        print("enter")\n'
        '    def __exit__(self, t, e, tb):\n'
        '        print("exit")\n'
        'def f():\n'
        '    with Guard():\n'
        '        return 1\n'
        'print(f())\n',
        'enter\nexit\n1\n',
    ),
    # ── 注解（只解析，不产生代码）──
    (
        '参数与返回注解被忽略',
        'def f(a: int, b: str = "x") -> int:\n'
        '    return a\n'
        'print(f(3))\n'
        'print(f(4, b="y"))\n',
        '3\n4\n',
    ),
    (
        '类属性注解',
        'class A:\n'
        '    x: int = 1\n'
        '    y: str = "s"\n'
        'print(A.x)\n'
        'print(A().y)\n',
        '1\ns\n',
    ),
    (
        '变量注解',
        'n: int = 5\n'
        'print(n)\n'
        'names: list = [1, 2]\n'
        'print(names)\n',
        '5\n[1, 2]\n',
    ),
    (
        '嵌套泛型注解',
        'def f(a: dict[str, int]) -> list[int]:\n'
        '    return [1]\n'
        'print(f({"a": 1}))\n',
        '[1]\n',
    ),
    # ── type() / isinstance() ──
    (
        'type 返回的类型名',
        'print(type(1))\n'
        'print(type("a"))\n'
        'print(type([1]))\n'
        'print(type(None))\n'
        'print(type(True))\n',
        "<class 'int'>\n<class 'str'>\n<class 'list'>\n"
        "<class 'NoneType'>\n<class 'bool'>\n",
    ),
    (
        'type(x) is T',
        'x = 1\n'
        'print(type(x) is int)\n'
        'print(type(x) is str)\n'
        'print(type("a") is str)\n',
        'True\nFalse\nTrue\n',
    ),
    (
        'isinstance 基础类型',
        'print(isinstance(1, int))\n'
        'print(isinstance("a", str))\n'
        'print(isinstance(1, str))\n'
        'print(isinstance([1], list))\n',
        'True\nTrue\nFalse\nTrue\n',
    ),
    (
        'isinstance 认继承',
        'class A:\n'
        '    pass\n'
        'class B(A):\n'
        '    pass\n'
        'class C(B):\n'
        '    pass\n'
        'print(isinstance(A(), A))\n'
        'print(isinstance(B(), A))\n'
        'print(isinstance(C(), A))\n'
        'print(isinstance(A(), C))\n',
        'True\nTrue\nTrue\nFalse\n',
    ),
    (
        'type 脚本实例带 __main__ 前缀',
        'class A:\n'
        '    pass\n'
        'print(type(A()))\n',
        "<class '__main__.A'>\n",
    ),
    (
        'isinstance 认异常层级',
        'try:\n'
        '    raise ValueError("x")\n'
        'except ValueError as e:\n'
        '    print(isinstance(e, ValueError))\n'
        '    print(isinstance(e, Exception))\n',
        'True\nTrue\n',
    ),
    (
        '__exit__ 的 exc_type 能 is 比较',
        'class S:\n'
        '    def __enter__(self):\n'
        '        return self\n'
        '    def __exit__(self, t, e, tb):\n'
        '        print(t is ValueError)\n'
        '        return True\n'
        'with S():\n'
        '    raise ValueError("x")\n',
        'True\n',
    ),
    (
        '局部赋值遮蔽类型名',
        'int = 5\n'
        'print(int)\n'
        'print(int + 1)\n'
        'print(type(1) is int)\n',
        "5\n6\nFalse\n",
    ),
    (
        '参数遮蔽类型名',
        'def f(str):\n'
        '    return str\n'
        'print(f(42))\n',
        '42\n',
    ),
    (
        '先读类型名后赋值不互相影响',
        'print(type(1) is int)\n'
        'int = 5\n'
        'print(int)\n'
        'print(type(1) is int)\n',
        'True\n5\nFalse\n',
    ),
    # ── 元组解包 ──
    (
        'for 元组解包',
        'pairs = [[1, "x"], [2, "y"]]\n'
        'for a, b in pairs:\n'
        '    print(a, b)\n',
        '1 x\n2 y\n',
    ),
    (
        '赋值元组解包',
        'a, b = 1, 2\n'
        'print(a)\n'
        'print(b)\n'
        'x, y = 3, 4\n'
        'x, y = y, x\n'
        'print(x, y)\n',
        '1\n2\n4 3\n',
    ),
    (
        'enumerate 与 zip 配合解包',
        'for i, v in enumerate(["a", "b"]):\n'
        '    print(i, v)\n'
        'for n, s in zip([1, 2], ["x", "y"]):\n'
        '    print(n, s)\n',
        '0 a\n1 b\n1 x\n2 y\n',
    ),
    (
        '解包长度不符抛 ValueError',
        'try:\n'
        '    a, b = [1]\n'
        'except ValueError:\n'
        '    print("bad")\n',
        'bad\n',
    ),
    # ── 基础内建 ──
    (
        'sorted 与 reversed',
        'print(sorted([3, 1, 2]))\n'
        'print(sorted(["b", "a"]))\n'
        'print(reversed([1, 2, 3]))\n',
        "[1, 2, 3]\n['a', 'b']\n[3, 2, 1]\n",
    ),
    (
        'any 与 all',
        'print(any([0, 1]))\n'
        'print(all([1, 2]))\n'
        'print(any([]))\n'
        'print(all([]))\n',
        'True\nTrue\nFalse\nTrue\n',
    ),
    (
        'chr / ord 往返',
        'print(chr(65))\n'
        'print(ord("A"))\n'
        'print(chr(0x4E2D))\n'
        'print(ord("中"))\n',
        'A\n65\n中\n20013\n',
    ),
    (
        'hex / bin / oct',
        'print(hex(255))\n'
        'print(bin(5))\n'
        'print(oct(8))\n'
        'print(hex(-255))\n'
        'print(hex(0))\n',
        '0xff\n0b101\n0o10\n-0xff\n0x0\n',
    ),
    (
        'repr 与 str 的区别',
        'print(repr("a"))\n'
        'print(str("a"))\n'
        'print(repr([1, "a"]))\n'
        'print(repr((1,)))\n'
        'print(repr(None))\n',
        "'a'\na\n[1, 'a']\n(1,)\nNone\n",
    ),
    (
        'divmod 与 pow',
        'print(divmod(7, 2))\n'
        'print(divmod(-7, 2))\n'
        'print(pow(2, 10))\n'
        'print(pow(2, 0))\n',
        '(3, 1)\n(-4, 1)\n1024\n1\n',
    ),
    (
        'tuple 构造',
        'print(tuple([1, 2]))\n'
        'print(tuple())\n'
        'print(tuple("ab"))\n',
        "(1, 2)\n()\n('a', 'b')\n",
    ),
    (
        'isinstance 元组形式',
        'class A:\n'
        '    pass\n'
        'class B:\n'
        '    pass\n'
        'print(isinstance(1, (str, int)))\n'
        'print(isinstance([], (str, int)))\n'
        'print(isinstance(A(), (A, B)))\n'
        'print(isinstance(1, ()))\n',
        'True\nFalse\nTrue\nFalse\n',
    ),
    # ── 内建的关键字实参 ──
    (
        'print 的 sep 与 end',
        'print(1, 2, sep="-")\n'
        'print("a", end="!")\n'
        'print("b")\n',
        '1-2\na!b\n',
    ),
    (
        'sorted 的 reverse 关键字',
        'print(sorted([2, 1, 3], reverse=True))\n'
        'print(sorted([2, 1, 3]))\n',
        '[3, 2, 1]\n[1, 2, 3]\n',
    ),
    (
        'int 的 base 关键字',
        'print(int("ff", base=16))\n'
        'print(int("10", base=2))\n'
        'print(int("-ff", base=16))\n',
        '255\n2\n-255\n',
    ),
    (
        'round 的 ndigits 关键字',
        'print(round(1.234, ndigits=2))\n'
        'print(round(1.6))\n',
        '1.23\n2\n',
    ),
    # ── 高阶函数与生成器消费 ──
    (
        'map 调脚本函数与 lambda',
        'def sq(x):\n'
        '    return x * x\n'
        'print(list(map(sq, [1, 2, 3])))\n'
        'print(list(map(lambda x: x + 10, [1, 2])))\n',
        '[1, 4, 9]\n[11, 12]\n',
    ),
    (
        'map 调实例方法',
        'class A:\n'
        '    def dbl(self, x):\n'
        '        return x * 2\n'
        'a = A()\n'
        'print(list(map(a.dbl, [1, 2])))\n',
        '[2, 4]\n',
    ),
    (
        'map 的内建与类型回调',
        'print(list(map(len, ["a", "bb", "ccc"])))\n'
        'print(list(map(str, [1, 2])))\n',
        "[1, 2, 3]\n['1', '2']\n",
    ),
    (
        'filter 谓词与 None',
        'def even(x):\n'
        '    return x % 2 == 0\n'
        'print(list(filter(even, [1, 2, 3, 4])))\n'
        'print(list(filter(None, [0, 1, "", "a"])))\n',
        "[2, 4]\n[1, 'a']\n",
    ),
    (
        'map 吃生成器',
        'def g():\n'
        '    yield 1\n'
        '    yield 2\n'
        'print(list(map(lambda x: x * 10, g())))\n',
        '[10, 20]\n',
    ),
    (
        'sum 吃生成器与 start',
        'def g():\n'
        '    yield 1\n'
        '    yield 2\n'
        '    yield 3\n'
        'print(sum(g()))\n'
        'print(sum([1, 2], 10))\n'
        'print(sum([1, 2], start=100))\n',
        '6\n13\n103\n',
    ),
    (
        'sorted 的 key',
        'def last(s):\n'
        '    return s[-1:]\n'
        'print(sorted(["xa", "yb", "zc"], key=last))\n'
        'print(sorted(["bb", "a", "ccc"], key=len))\n'
        'print(sorted([3, 1, 2], key=lambda x: -x))\n',
        "['xa', 'yb', 'zc']\n['a', 'bb', 'ccc']\n[3, 2, 1]\n",
    ),
    (
        '内建函数是一等值',
        'f = len\n'
        'print(f("abc"))\n'
        'print(len)\n'
        'print(type(f))\n',
        "3\n<built-in function len>\n<class 'builtin_function_or_method'>\n",
    ),
    (
        '内建函数值的名字',
        'f = len\n'
        'print(len)\n'
        'print(f)\n'
        'print(str)\n',
        "<built-in function len>\n"
        "<built-in function len>\n"
        "<class 'str'>\n",
    ),
    (
        '异常类可调用',
        'e = ValueError("boom")\n'
        'print(isinstance(e, ValueError))\n'
        'print(e)\n'
        'try:\n'
        '    raise TypeError("t")\n'
        'except TypeError as exc:\n'
        '    print(exc)\n',
        "True\nboom\nt\n",
    ),
    # ── 字符串 / 列表 / 字典方法 ──
    (
        '字符串补齐与判定',
        'print("ab".ljust(5) + "|")\n'
        'print("ab".rjust(5) + "|")\n'
        'print("ab".center(6) + "|")\n'
        'print("42".zfill(5))\n'
        'print("-7".zfill(4))\n'
        'print("hello world".title())\n'
        'print("123".isdigit(), "12a".isdigit())\n'
        'print("abc".isalpha(), "abc".isupper(), "ABC".isupper())\n',
        'ab   |\n'
        '   ab|\n'
        '  ab  |\n'
        '00042\n'
        '-007\n'
        'Hello World\n'
        'True False\n'
        'True False True\n',
    ),
    (
        '字符串拆分与修剪',
        'print("a\\nb\\r\\nc".splitlines())\n'
        'print("a\\n".splitlines())\n'
        'print("  x  ".lstrip() + "|")\n'
        'print("  x  ".rstrip() + "|")\n',
        "['a', 'b', 'c']\n['a']\nx  |\n  x|\n",
    ),
    (
        '列表 index / remove / copy / count',
        'a = [1, 2, 3, 2]\n'
        'print(a.index(2))\n'
        'print(a.count(2))\n'
        'b = a.copy()\n'
        'b.append(9)\n'
        'print(a, b)\n'
        'a.remove(2)\n'
        'print(a)\n'
        'try:\n'
        '    a.remove(99)\n'
        'except ValueError as err:\n'
        '    print("remove:", err)\n'
        'try:\n'
        '    [1].index(5)\n'
        'except ValueError as err:\n'
        '    print("index:", err)\n',
        '1\n2\n[1, 2, 3, 2] [1, 2, 3, 2, 9]\n[1, 3, 2]\n'
        'remove: list.remove(x): x not in list\n'
        'index: 5 is not in list\n',
    ),
    (
        '列表 sort 的关键字',
        'x = [3, 1, 2]\n'
        'x.sort()\n'
        'print(x)\n'
        'x.sort(reverse=True)\n'
        'print(x)\n'
        'words = ["bb", "a", "ccc"]\n'
        'words.sort(key=len)\n'
        'print(words)\n',
        "[1, 2, 3]\n[3, 2, 1]\n['a', 'bb', 'ccc']\n",
    ),
    (
        '字典 setdefault / copy / clear',
        'd = {"a": 1}\n'
        'print(d.setdefault("a", 9))\n'
        'print(d.setdefault("b", 2))\n'
        'print(d)\n'
        'e = d.copy()\n'
        'e["c"] = 3\n'
        'print(d, e)\n'
        'd.clear()\n'
        'print(d)\n',
        "1\n2\n{'a': 1, 'b': 2}\n"
        "{'a': 1, 'b': 2} {'a': 1, 'b': 2, 'c': 3}\n{}\n",
    ),
    (
        'str.format 多参数与格式符',
        'print("{} and {}".format(1, 2))\n'
        'print("{0} {1} {0}".format("a", "b"))\n'
        'print("{:.2f}".format(3.14159))\n'
        'print("{:05d}".format(42))\n'
        'print("{:.3s}".format("abcdef"))\n'
        'print("{:>6}|".format("ab"))\n'
        'print("{:<6}|".format("ab"))\n'
        'print("{:^6}|".format("ab"))\n'
        'print("{:+.2f}".format(1.5))\n'
        'print("{:x}".format(255))\n'
        'print("literal {{}} braces".format())\n',
        '1 and 2\n'
        'a b a\n'
        '3.14\n'
        '00042\n'
        'abc\n'
        '    ab|\n'
        'ab    |\n'
        '  ab  |\n'
        '+1.50\n'
        'ff\n'
        'literal {} braces\n',
    ),
    (
        'dict 的带参构造',
        'print(dict())\n'
        'print(dict([["a", 1], ["b", 2]]))\n'
        'print(dict([("a", 1)]))\n'
        'd = {"x": 9}\n'
        'print(dict(d))\n'
        'print(dict(a=1, b=2))\n',
        "{}\n{'a': 1, 'b': 2}\n{'a': 1}\n{'x': 9}\n{'a': 1, 'b': 2}\n",
    ),
    (
        'min / max 的 key 关键字',
        'print(min([3, 1, 2]))\n'
        'print(max([3, 1, 2]))\n'
        'print(min(3, 1, 2))\n'
        'print(min(["bb", "a", "ccc"], key=len))\n'
        'print(max(["bb", "a", "ccc"], key=len))\n',
        '1\n3\n1\na\nccc\n',
    ),
    (
        'dir 列出可用方法',
        'names = dir("")\n'
        'print("upper" in names, "append" in names)\n'
        'names = dir([1])\n'
        'print("append" in names, "sort" in names, "nope" in names)\n'
        'names = dir({})\n'
        'print("get" in names, "setdefault" in names)\n',
        'True False\nTrue True False\nTrue True\n',
    ),
    (
        'f-string 格式说明符',
        'x = 3.14159\n'
        'print(f"{x:.2f}")\n'
        'print(f"{x:>8.2f}|")\n'
        'n = 42\n'
        'print(f"{n:05d}")\n'
        'print(f"{n:x}")\n'
        'print(f"{n:d} and {x:.1f}")\n',
        '3.14\n    3.14|\n00042\n2a\n42 and 3.1\n',
    ),
    (
        'while else',
        'i = 0\n'
        'while i < 2:\n'
        '    print("w", i)\n'
        '    i += 1\n'
        'else:\n'
        '    print("while else")\n'
        'j = 0\n'
        'while j < 2:\n'
        '    if j == 1:\n'
        '        break\n'
        '    j += 1\n'
        'else:\n'
        '    print("never")\n',
        'w 0\nw 1\nwhile else\n',
    ),
    (
        'for else',
        'for x in [1, 2]:\n'
        '    print("f", x)\n'
        'else:\n'
        '    print("for else")\n'
        'for x in [1, 2]:\n'
        '    if x == 2:\n'
        '        break\n'
        '    print("g", x)\n'
        'else:\n'
        '    print("never")\n'
        'for x in []:\n'
        '    print("empty")\n'
        'else:\n'
        '    print("empty else")\n',
        'f 1\nf 2\nfor else\ng 1\nempty else\n',
    ),
    (
        'f-string 转换标志',
        'x = "ab"\n'
        'n = 3.14159\n'
        'print(f"{x!r}")\n'
        'print(f"{x!s}")\n'
        'print(f"{x!r:>8}|")\n'
        'print(f"{n:.2f}")\n'
        'print(f"{[1, 2]!r}")\n',
        "\'ab\'\nab\n    \'ab\'|\n3.14\n[1, 2]\n",
    ),
    (
        'encode / decode',
        's = "abc"\n'
        'b = s.encode()\n'
        'print(b)\n'
        'print(b.decode())\n'
        't = "\xe4\xb8\xad"\n'
        'print(t.encode().decode() == t)\n'
        'print(b"xyz".decode())\n',
        "b\'abc\'\nabc\nTrue\nxyz\n",
    ),
    (
        '位置-only 参数',
        'def f(a, b, /, c):\n'
        '    return a + b + c\n'
        'print(f(1, 2, 3))\n'
        'print(f(1, 2, c=3))\n'
        'def g(x, /):\n'
        '    return x * 2\n'
        'print(g(5))\n',
        '6\n6\n10\n',
    ),
    (
        '嵌套函数与闭包',
        'def outer(x):\n'
        '    def inner(y):\n'
        '        return x + y\n'
        '    return inner(10)\n'
        'print(outer(5))\n'
        'def make_adder(a):\n'
        '    def add(b):\n'
        '        return a + b\n'
        '    return add\n'
        'f = make_adder(3)\n'
        'g = make_adder(100)\n'
        'print(f(4))\n'
        'print(g(4))\n',
        '15\n7\n104\n',
    ),
    (
        'nonlocal 计数',
        'def counter():\n'
        '    n = 0\n'
        '    def step():\n'
        '        nonlocal n\n'
        '        n += 1\n'
        '        return n\n'
        '    return step\n'
        'c = counter()\n'
        'print(c())\n'
        'print(c())\n'
        'print(c())\n',
        '1\n2\n3\n',
    ),
    (
        '嵌套递归',
        'def fact(n):\n'
        '    def go(k):\n'
        '        if k <= 1:\n'
        '            return 1\n'
        '        return k * go(k - 1)\n'
        '    return go(n)\n'
        'print(fact(5))\n',
        '120\n',
    ),
    (
        '海象运算符',
        'n = 0\n'
        'if (n := 5) > 3:\n'
        '    print(n)\n'
        'xs = [1, 2, 3]\n'
        'if (m := len(xs)) > 2:\n'
        '    print(m)\n',
        '5\n3\n',
    ),
    (
        'match 语句',
        'def name(cmd):\n'
        '    match cmd:\n'
        '        case "start":\n'
        '            return "starting"\n'
        '        case "stop":\n'
        '            return "stopping"\n'
        '        case _:\n'
        '            return "unknown"\n'
        'print(name("start"))\n'
        'print(name("stop"))\n'
        'print(name("jump"))\n'
        'def size(n):\n'
        '    match n:\n'
        '        case 1 | 2 | 3:\n'
        '            return "small"\n'
        '        case -1:\n'
        '            return "minus"\n'
        '        case _:\n'
        '            return "many"\n'
        'print(size(2))\n'
        'print(size(-1))\n'
        'print(size(9))\n',
        'starting\nstopping\nunknown\nsmall\nminus\nmany\n',
    ),
    (
        'match 守卫与捕获',
        'def check(v):\n'
        '    match v:\n'
        '        case 1 if v > 10:\n'
        '            return "big one"\n'
        '        case 1:\n'
        '            return "one"\n'
        '        case other:\n'
        '            return "other:" + str(other)\n'
        'print(check(1))\n'
        'print(check(5))\n'
        'def band(v):\n'
        '    match v:\n'
        '        case 1 if v > 100:\n'
        '            return "huge one"\n'
        '        case n if n > 50:\n'
        '            return "big"\n'
        '        case other:\n'
        '            return "small:" + str(other)\n'
        'print(band(1))\n'
        'print(band(500))\n'
        'print(band(7))\n'
        'def grab(v):\n'
        '    match v:\n'
        '        case 7 as seven:\n'
        '            return seven * 2\n'
        '        case _:\n'
        '            return 0\n'
        'print(grab(7))\n'
        'print(grab(3))\n',
        'one\nother:5\nsmall:1\nbig\nsmall:7\n14\n0\n',
    ),
    (
        'Ellipsis 字面量',
        'x = ...\n'
        'print(x)\n'
        'print(x is Ellipsis)\n'
        'print(... is None)\n'
        'print(type(...))\n'
        'print(len((..., ...)))\n'
        'print(repr(...))\n',
        'Ellipsis\nTrue\nFalse\n<class \'ellipsis\'>\n2\nEllipsis\n',
    ),
    (
        'yield from',
        'def inner():\n'
        '    yield 1\n'
        '    yield 2\n'
        'def outer():\n'
        '    yield 0\n'
        '    yield from inner()\n'
        '    yield 3\n'
        'for v in outer():\n'
        '    print(v)\n'
        'print(list(outer()))\n'
        'def chain():\n'
        '    yield from [10, 20]\n'
        '    yield from [30]\n'
        'print(list(chain()))\n',
        '0\n1\n2\n3\n[0, 1, 2, 3]\n[10, 20, 30]\n',
    ),
    (
        'yield from 惰性',
        'def inner():\n'
        '    yield 1\n'
        '    yield 2\n'
        'def outer():\n'
        '    print("outer start")\n'
        '    yield from inner()\n'
        '    print("outer end")\n'
        'g = outer()\n'
        'print("created")\n'
        'print(next(g))\n'
        'print(next(g))\n'
        'try:\n'
        '    print(next(g))\n'
        'except Exception:\n'
        '    print("done")\n',
        'created\nouter start\n1\n2\nouter end\ndone\n',
    ),
    (
        '类型别名与矩阵乘',
        'type Names = list\n'
        'x = Names()\n'
        'x.append(1)\n'
        'print(x)\n'
        'print(Names is list)\n'
        'try:\n'
        '    y = 1 @ 2\n'
        'except TypeError as e:\n'
        '    print("TypeError", e)\n',
        "[1]\nTrue\nTypeError unsupported operand type(s) for @: 'int' "
        "and 'int'\n",
    ),
    # ── id / hash ──
    (
        'id 对同一对象稳定',
        'a = [1]\n'
        'b = a\n'
        'print(id(a) == id(b))\n'
        'print(id(a) == id([1]))\n',
        'True\nFalse\n',
    ),
    (
        'hash 与相等性一致',
        'print(hash(1) == hash(1))\n'
        'print(hash("a") == hash("a"))\n'
        'print(hash(1) == hash(1.0))\n'
        'd = {}\n'
        'd[1] = "x"\n'
        'print(d[1])\n',
        'True\nTrue\nTrue\nx\n',
    ),
    (
        'hash 拒绝不可哈希类型',
        'try:\n'
        '    hash([1])\n'
        'except TypeError:\n'
        '    print("unhashable")\n',
        'unhashable\n',
    ),
    # ── 期望被拒绝的 ──
    (
        '两个 Activity 被拒绝',
        'class A(Activity):\n'
        '    x = 1\n'
        'class B(Activity):\n'
        '    y = 2\n',
        None,
    ),
    (
        '方法首参不是 self',
        'class App(Activity):\n'
        '    def f(other):\n'
        '        return 1\n',
        None,
    ),
    (
        '方法里漏写 global 的笔误仍被拒绝',
        'class App(Activity):\n'
        '    v = 0\n'
        '    def f(self):\n'
        '        v = 2\n',
        None,
    ),
    (
        '类体里不许有语句',
        'class App(Activity):\n'
        '    print(1)\n',
        None,
    ),
]


def main(argv):
    if len(argv) < 3:
        print('用法: check_activity.py <pcc.exe> <aipy_run.exe>',
              file=sys.stderr)
        return 2
    pcc = os.path.abspath(argv[1])
    aipy_run = os.path.abspath(argv[2])
    for path in (pcc, aipy_run):
        if not os.path.isfile(path):
            print('找不到 %s' % path, file=sys.stderr)
            return 2

    failures = []
    scratch = tempfile.mkdtemp(prefix='aipy_activity_')
    for index, (name, source, expected) in enumerate(CASES):
        if expected is None:
            error = reject(pcc, source, scratch, 'reject_%d' % index)
            print('  ✓ %-28s 被拒绝（%s）' % (name, error.split('error: ')[-1][:34]))
            continue
        actual = run(pcc, aipy_run, source, scratch, 'case_%d' % index)
        if actual != expected:
            failures.append('%s：期望 %r，实际 %r' % (name, expected, actual))
            print('  ✗ %-28s 输出不符' % name)
        else:
            print('  ✓ %-28s %s' % (name, expected.strip()))

    if failures:
        print('\n失败：', file=sys.stderr)
        for line in failures:
            print('  ' + line, file=sys.stderr)
        return 1
    print('\nActivity 展开路径全部通过（%d 条）。' % len(CASES))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
