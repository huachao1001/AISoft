#!/usr/bin/env python3
"""探针：逐条实测 AiPy 支持哪些 Python 语法。

这个脚本不读源码、不看注释，只做一件事：把每条写法编成字节码、跑一遍，
把**真实结果**记下来。文档说支持的，必须在这里真的能跑；文档说不支持的，
必须在这里真的报错。

输出直接用于生成 LANGUAGE.md 的表格。改了编译器就重跑一遍，别靠记。

── 为什么必须真的运行 ──

方法名是**运行期**查表的：`"a".splitlines()` 编译时只是 CALL_METHOD +
常量 "splitlines"，要到 VM 里才发现「字符串没有这个方法」。只看编译结果
会把一批不支持的方法误判成支持。

用法：probe_syntax.py <pcc.exe> [aipy_run.exe]
"""

import io
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))


def probe(pcc, source, run_exe, scratch):
    """返回 (能否用, 错误信息)。给了 run_exe 就实际跑一遍。"""
    script = os.path.join(scratch, 'probe.py')
    with open(script, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(source)
    path = os.path.join(scratch, 'probe.bc')

    compiled = subprocess.run([pcc, script, path], capture_output=True,
                              text=True, encoding='utf-8', errors='replace')
    if compiled.returncode != 0:
        text = compiled.stderr.strip()
        marker = 'error: '
        if marker in text:
            text = text.split(marker, 1)[1]
        return (False, text or '编译失败（退出码 %d）' % compiled.returncode)

    if run_exe is None:
        return (True, '')

    completed = subprocess.run([run_exe, path], capture_output=True, text=True,
                               encoding='utf-8')
    if completed.returncode != 0:
        text = completed.stderr.strip()
        # 去掉 `probe.bc:行: ` 前缀，只留原因
        for marker in ('.bc:', 'probe:'):
            if marker in text:
                text = text.split(marker, 1)[1]
                text = text.split(': ', 1)[-1]
                break
        return (False, text or '运行失败（退出码 %d）' % completed.returncode)
    return (True, '')


# 每条用例：{类别, 写法, 源码, 期望}
#   期望 'ok'   = 应当能编译并跑通
#   期望 'no'   = 应当被拒绝（编译期或运行期）
#   期望 'skip' = 不判断（需要外部环境，比如扩展函数）
CASES = [
    # ── 字面量与常量 ──
    ('字面量', '整数', 'x = 42\n', 'ok'),
    ('字面量', '负数', 'x = -42\n', 'ok'),
    ('字面量', '大整数', 'x = 1234567890123\n', 'ok'),
    ('字面量', '浮点', 'x = 3.14\n', 'ok'),
    ('字面量', '科学计数法', 'x = 1.5e10\n', 'ok'),
    ('字面量', '字符串', 'x = "hello"\n', 'ok'),
    ('字面量', '三引号字符串', 'x = """多行\n文本"""\n', 'ok'),
    ('字面量', '布尔 True', 'x = True\n', 'ok'),
    ('字面量', '布尔 False', 'x = False\n', 'ok'),
    ('字面量', 'None', 'x = None\n', 'ok'),
    ('字面量', '列表', 'x = [1, 2, 3]\n', 'ok'),
    ('字面量', '空列表', 'x = []\n', 'ok'),
    ('字面量', '嵌套列表', 'x = [[1, 2], [3, 4]]\n', 'ok'),
    ('字面量', '字典', 'x = {"a": 1}\n', 'ok'),
    ('字面量', '空字典', 'x = {}\n', 'ok'),
    ('字面量', '元组', 'x = (1, 2)\n', 'ok'),
    ('字面量', '单元素元组', 'x = (1,)\n', 'ok'),
    ('字面量', '空元组', 'x = ()\n', 'ok'),
    ('字面量', '元组下标', 'x = (1, 2)[0]\n', 'ok'),
    ('字面量', '元组相加', 'x = (1,) + (2,)\n', 'ok'),
    ('字面量', '集合', 'x = {1, 2, 3}\n', 'ok'),
    ('字面量', '集合并', 'x = {1, 2} | {3}\n', 'ok'),
    ('字面量', '集合交', 'x = {1, 2} & {2}\n', 'ok'),
    ('字面量', 'set()', 'x = set()\n', 'ok'),
    ('字面量', 'f-string', 'x = f"{1+1}"\n', 'ok'),
    ('字面量', 'f-string 文本', 'n = 1\nx = f"n={n}"\n', 'ok'),
    ('字面量', 'f-string 双花括号', 'x = f"{{a}}"\n', 'ok'),
    ('字面量', 'f-string 空', 'x = f""\n', 'ok'),
    ('字面量', 'f-string 嵌套下标', 'd = {"k": 1}\nx = f"{d[\'k\']}"\n', 'ok'),
    ('字面量', 'f-string 格式说明符', 'x = f"{1:>5}"\n', 'no'),
    ('字面量', 'bytes', 'x = b"abc"\n', 'ok'),
    ('字面量', 'bytes 空', 'x = b""\n', 'ok'),
    ('字面量', 'bytes 转义', 'x = b"\\x00\\xff"\n', 'ok'),
    ('字面量', 'bytes 拼接不报错', 'x = b"a" b"b"\n', 'ok'),
    ('字面量', 'bytes 与 str 混拼', 'x = b"a" "b"\n', 'no'),
    ('字面量', 'Ellipsis', 'x = ...\n', 'no'),

    # ── 运算符 ──
    ('运算符', '加法', 'x = 1 + 2\n', 'ok'),
    ('运算符', '减法', 'x = 1 - 2\n', 'ok'),
    ('运算符', '乘法', 'x = 2 * 3\n', 'ok'),
    ('运算符', '除法', 'x = 7 / 2\n', 'ok'),
    ('运算符', '整除', 'x = 7 // 2\n', 'ok'),
    ('运算符', '取模', 'x = 7 % 2\n', 'ok'),
    ('运算符', '幂', 'x = 2 ** 10\n', 'ok'),
    ('运算符', '位与', 'x = 5 & 3\n', 'ok'),
    ('运算符', '位或', 'x = 5 | 3\n', 'ok'),
    ('运算符', '位异或', 'x = 5 ^ 3\n', 'ok'),
    ('运算符', '左移', 'x = 1 << 4\n', 'ok'),
    ('运算符', '右移', 'x = 16 >> 2\n', 'ok'),
    ('运算符', '按位取反', 'x = ~5\n', 'ok'),
    ('运算符', '负号', 'x = -5\n', 'ok'),
    ('运算符', 'not', 'x = not True\n', 'ok'),
    ('运算符', '矩阵乘 @', 'x = a @ b\n', 'no'),
    ('运算符', '海象 :=', 'if (n := 5) > 0:\n    x = n\n', 'no'),

    # ── 比较与逻辑 ──
    ('比较', '等于', 'x = 1 == 1\n', 'ok'),
    ('比较', '不等于', 'x = 1 != 2\n', 'ok'),
    ('比较', '小于', 'x = 1 < 2\n', 'ok'),
    ('比较', '小于等于', 'x = 1 <= 2\n', 'ok'),
    ('比较', '大于', 'x = 2 > 1\n', 'ok'),
    ('比较', '大于等于', 'x = 2 >= 1\n', 'ok'),
    ('比较', 'in', 'x = 1 in [1, 2]\n', 'ok'),
    ('比较', 'not in', 'x = 3 not in [1, 2]\n', 'ok'),
    ('比较', '链式比较', 'x = 1 < 2 < 3\n', 'ok'),
    ('比较', 'is', 'x = a is b\n', 'ok'),
    ('比较', 'is not', 'x = a is not b\n', 'ok'),
    ('逻辑', 'and', 'x = True and False\n', 'ok'),
    ('逻辑', 'or', 'x = True or False\n', 'ok'),
    ('逻辑', '三元表达式', 'x = 1 if True else 2\n', 'ok'),

    # ── 语句 ──
    ('语句', '赋值', 'x = 1\n', 'ok'),
    ('语句', '链式赋值', 'a = b = 1\n', 'ok'),
    ('语句', '增量赋值 +=', 'x = 1\nx += 1\n', 'ok'),
    ('语句', '增量赋值 -=', 'x = 1\nx -= 1\n', 'ok'),
    ('语句', '增量赋值 *=', 'x = 2\nx *= 3\n', 'ok'),
    ('语句', '类型注解赋值', 'x: int = 1\n', 'ok'),
    ('语句', '纯注解无值', 'x: int\n', 'ok'),
    ('语句', '链式下标赋值', 'x = [1]\nx[0] = 2\n', 'ok'),
    ('语句', 'pass', 'pass\n', 'ok'),
    ('语句', 'del', 'x = 1\ndel x\n', 'ok'),
    ('语句', 'del 多个', 'x = 1\ny = 2\ndel x, y\n', 'ok'),
    ('语句', 'del 后读取', 'x = 1\ndel x\ntry:\n    y = x\nexcept NameError:\n    y = 0\n', 'ok'),
    ('语句', 'assert', 'assert 1 == 1\n', 'ok'),
    ('语句', 'assert 带消息', 'assert 1 == 1, "boom"\n', 'ok'),
    ('语句', 'assert 失败被接住', 'try:\n    assert 1 == 2\nexcept AssertionError:\n    pass\n', 'ok'),
    ('语句', 'raise 类型', 'raise ValueError\n', 'ok'),
    ('语句', 'raise 带消息', 'raise ValueError("x")\n', 'ok'),
    ('语句', 'bare raise', 'try:\n    x = 1\nexcept ValueError:\n    raise\n', 'ok'),
    ('语句', 'raise 非异常类', 'raise 1\n', 'ok'),
    ('语句', 'try/except', 'try:\n    x = 1\nexcept:\n    x = 2\n', 'ok'),
    ('语句', 'try/except as', 'try:\n    x = 1\nexcept ValueError as e:\n    x = 2\n', 'ok'),
    ('语句', 'try/except 元组', 'try:\n    x = 1\nexcept (ValueError, KeyError):\n    x = 2\n', 'ok'),
    ('语句', 'try/else/finally',
     'try:\n    x = 1\nexcept ValueError:\n    x = 2\nelse:\n    x = 3\nfinally:\n    x = 4\n',
     'ok'),
    ('语句', 'try 只有 finally', 'try:\n    x = 1\nfinally:\n    x = 2\n', 'ok'),
    ('语句', 'try 无 except 无 finally', 'try:\n    x = 1\n', 'no',
     "'try' must be followed by 'except' or 'finally'"),
    ('语句', 'except 类型不是异常类',
     'try:\n    x = 1\nexcept 1:\n    x = 2\n', 'no', '"<expr>" is not an exception type'),
    ('语句', 'except 未知异常类',
     'try:\n    x = 1\nexcept NotAnException:\n    x = 2\n', 'no',
     '"NotAnException" is not an exception type'),
    ('语句', 'with', 'x = [1]\nwith x:\n    y = 1\n', 'no', '没有方法 __enter__'),
    ('语句', 'with 多项', 'x = [1]\nwith x, x:\n    y = 1\n', 'no',
     'only one item is allowed'),
    ('语句', 'with as', 'x = [1]\nwith x as v:\n    y = 1\n', 'no', '没有方法 __enter__'),
    ('语句', 'match', 'match 1:\n    case 1:\n        x = 1\n', 'no'),
    ('语句', 'global', 'x = 1\ndef f():\n    global x\n    x = 2\n', 'ok'),
    ('语句', 'nonlocal', 'def f():\n    y = 1\n    def g():\n        nonlocal y\n        y = 2\n', 'no'),
    ('语句', '类型别名 type', 'type X = int\n', 'no'),

    # ── 控制流 ──
    ('控制流', 'if', 'if True:\n    x = 1\n', 'ok'),
    ('控制流', 'if/elif/else', 'if False:\n    x = 1\nelif True:\n    x = 2\nelse:\n    x = 3\n', 'ok'),
    ('控制流', 'while', 'n = 0\nwhile n < 3:\n    n += 1\n', 'ok'),
    ('控制流', 'while/else', 'while False:\n    pass\nelse:\n    x = 1\n', 'no'),
    ('控制流', 'for in 列表', 'for i in [1, 2]:\n    x = i\n', 'ok'),
    ('控制流', 'for in range', 'for i in range(3):\n    x = i\n', 'ok'),
    ('控制流', 'for in 字符串', 'for c in "ab":\n    x = c\n', 'ok'),
    ('控制流', 'for in 字典', 'for k in {"a": 1}:\n    x = k\n', 'ok'),
    ('控制流', 'for/else', 'for i in [1]:\n    pass\nelse:\n    x = 1\n', 'no'),
    ('控制流', 'break', 'for i in [1, 2]:\n    break\n', 'ok'),
    ('控制流', 'continue', 'for i in [1, 2]:\n    continue\n', 'ok'),
    ('控制流', '嵌套循环', 'for i in [1]:\n    for j in [2]:\n        x = i\n', 'ok'),

    # ── 函数 ──
    ('函数', '定义与调用', 'def f():\n    return 1\nx = f()\n', 'ok'),
    ('函数', '带参数', 'def f(a, b):\n    return a + b\nx = f(1, 2)\n', 'ok'),
    ('函数', '递归', 'def f(n):\n    if n < 2:\n        return n\n    return f(n-1)\n', 'ok'),
    ('函数', '无返回', 'def f():\n    x = 1\ny = f()\n', 'ok'),
    ('函数', '默认参数', 'def f(a=1):\n    return a\n', 'no'),
    ('函数', '关键字传参', 'def f(a):\n    return a\nx = f(a=1)\n', 'no'),
    ('函数', '*args', 'def f(*args):\n    return 1\n', 'no'),
    ('函数', '**kwargs', 'def f(**kw):\n    return 1\n', 'no'),
    ('函数', '仅关键字参数', 'def f(*, a):\n    return a\n', 'no'),
    ('函数', '位置-only 参数', 'def f(a, /):\n    return a\n', 'no'),
    ('函数', '嵌套函数', 'def f():\n    def g():\n        return 1\n    return g()\n', 'no'),
    ('函数', 'lambda', 'f = lambda x: x + 1\n', 'no'),
    ('函数', '装饰器', '@dec\ndef f():\n    return 1\n', 'no'),
    ('函数', '生成器 yield', 'def f():\n    yield 1\n', 'no'),
    ('函数', 'async def', 'async def f():\n    return 1\n', 'no'),
    ('函数', '函数当值传递', 'def f():\n    return 1\ng = f\n', 'no'),
    ('函数', '文档字符串', 'def f():\n    """说明"""\n    return 1\n', 'ok'),
    ('函数', '类型注解返回值', 'def f() -> int:\n    return 1\n', 'ok'),

    # ── 字符串方法 ──
    ('字符串方法', 'upper', 'x = "a".upper()\n', 'ok'),
    ('字符串方法', 'lower', 'x = "A".lower()\n', 'ok'),
    ('字符串方法', 'strip', 'x = " a ".strip()\n', 'ok'),
    ('字符串方法', 'replace', 'x = "ab".replace("a", "c")\n', 'ok'),
    ('字符串方法', 'split', 'x = "a,b".split(",")\n', 'ok'),
    ('字符串方法', 'startswith', 'x = "ab".startswith("a")\n', 'ok'),
    ('字符串方法', 'endswith', 'x = "ab".endswith("b")\n', 'ok'),
    ('字符串方法', 'find', 'x = "ab".find("b")\n', 'ok'),
    ('字符串方法', 'count', 'x = "aa".count("a")\n', 'ok'),
    ('字符串方法', 'join', 'x = "-".join(["a"])\n', 'ok'),
    ('字符串方法', 'format', 'x = "{}".format([1])\n', 'ok'),
    ('字符串方法', 'splitlines', 'x = "a\\nb".splitlines()\n', 'no'),
    ('字符串方法', 'title', 'x = "ab".title()\n', 'no'),
    ('字符串方法', 'zfill', 'x = "1".zfill(3)\n', 'no'),
    ('字符串方法', 'encode', 'x = "a".encode()\n', 'no'),

    # ── 列表方法 ──
    ('列表方法', 'append', 'x = []\nx.append(1)\n', 'ok'),
    ('列表方法', 'extend', 'x = []\nx.extend([1])\n', 'ok'),
    ('列表方法', 'insert', 'x = []\nx.insert(0, 1)\n', 'ok'),
    ('列表方法', 'pop', 'x = [1]\ny = x.pop()\n', 'ok'),
    ('列表方法', 'clear', 'x = [1]\nx.clear()\n', 'ok'),
    ('列表方法', 'sort', 'x = [2, 1]\nx.sort()\n', 'ok'),
    ('列表方法', 'reverse', 'x = [1, 2]\nx.reverse()\n', 'ok'),
    ('列表方法', 'index', 'x = [1].index(1)\n', 'no'),
    ('列表方法', 'remove', 'x = [1].remove(1)\n', 'no'),
    ('列表方法', 'copy', 'x = [1].copy()\n', 'no'),

    # ── 字典方法 ──
    ('字典方法', 'get', 'x = {"a": 1}.get("a")\n', 'ok'),
    ('字典方法', 'keys', 'x = {"a": 1}.keys()\n', 'ok'),
    ('字典方法', 'values', 'x = {"a": 1}.values()\n', 'ok'),
    ('字典方法', 'items', 'x = {"a": 1}.items()\n', 'ok'),
    ('字典方法', 'update', 'x = {}\nx.update({"a": 1})\n', 'ok'),
    ('字典方法', 'pop', 'x = {"a": 1}\ny = x.pop("a")\n', 'ok'),
    ('字典方法', 'setdefault', 'x = {}\ny = x.setdefault("a", 1)\n', 'no'),
    ('字典方法', 'copy', 'x = {"a": 1}.copy()\n', 'no'),

    # ── 下标与切片 ──
    ('下标切片', '正向下标', 'x = [1, 2][0]\n', 'ok'),
    ('下标切片', '负向下标', 'x = [1, 2][-1]\n', 'ok'),
    ('下标切片', '切片', 'x = [1, 2, 3][0:2]\n', 'ok'),
    ('下标切片', '省略起点', 'x = [1, 2, 3][:2]\n', 'ok'),
    ('下标切片', '省略终点', 'x = [1, 2, 3][1:]\n', 'ok'),
    ('下标切片', '负步长', 'x = [1, 2, 3][::-1]\n', 'ok'),
    ('下标切片', '切片赋值', 'x = [1, 2]\nx[0:1] = [3]\n', 'no'),
    ('下标切片', '字典下标', 'x = {"a": 1}\ny = x["a"]\n', 'ok'),

    # ── 内建函数 ──
    ('内建', 'len', 'x = len([1])\n', 'ok'),
    ('内建', 'str', 'x = str(1)\n', 'ok'),
    ('内建', 'int', 'x = int("1")\n', 'ok'),
    ('内建', 'float', 'x = float("1.5")\n', 'ok'),
    ('内建', 'bool', 'x = bool(1)\n', 'ok'),
    ('内建', 'abs', 'x = abs(-1)\n', 'ok'),
    ('内建', 'round', 'x = round(1.5, 0)\n', 'ok'),
    ('内建', 'min', 'x = min(1, 2)\n', 'ok'),
    ('内建', 'max', 'x = max([1, 2])\n', 'ok'),
    ('内建', 'sum', 'x = sum([1, 2])\n', 'ok'),
    ('内建', 'list', 'x = list("ab")\n', 'ok'),
    ('内建', 'dict', 'x = dict()\n', 'ok'),
    ('内建', 'range', 'x = range(3)\n', 'ok'),
    ('内建', 'print', 'print(1)\n', 'ok'),
    ('内建', 'format_text', 'x = format_text("{}", [1])\n', 'ok'),
    ('内建', 'parse_number', 'x = parse_number("1")\n', 'ok'),
    ('内建', 'char_from_code', 'x = char_from_code(65)\n', 'ok'),
    ('内建', 'text_of_char', 'x = text_of_char(65)\n', 'ok'),
    ('内建', 'sorted', 'x = sorted([2, 1])\n', 'no'),
    ('内建', 'enumerate', 'x = enumerate([1])\n', 'no'),
    ('内建', 'zip', 'x = zip([1], [2])\n', 'no'),
    ('内建', 'map', 'x = map(str, [1])\n', 'no'),
    ('内建', 'filter', 'x = filter(bool, [1])\n', 'no'),
    ('内建', 'any', 'x = any([1])\n', 'no'),
    ('内建', 'all', 'x = all([1])\n', 'no'),
    ('内建', 'reversed', 'x = reversed([1])\n', 'no'),
    ('内建', 'isinstance', 'x = isinstance(1, int)\n', 'no'),
    ('内建', 'type', 'x = type(1)\n', 'no'),
    ('内建', 'input', 'x = input()\n', 'no'),
    ('内建', 'open', 'x = open("f")\n', 'no'),
    ('内建', 'help', 'help()\n', 'no'),
    ('内建', 'sum 当值使用', 'f = len\n', 'no'),

    # ── 模块与类 ──
    ('模块类', 'import', 'import math\n', 'ok'),
    ('模块类', 'from import', 'from math import pi\n', 'ok'),
    ('模块类', 'import as', 'import math as m\n', 'ok'),
    ('模块类', '普通 class', 'class A:\n    x = 1\n', 'no'),
    ('模块类', 'class 继承 Activity', 'class App(Activity):\n    def on_start(self):\n        self.x = 1\non_start()\n', 'ok'),
    ('模块类', 'class 继承其它', 'class A(Base):\n    x = 1\n', 'no'),
    ('模块类', '属性访问 self.x', 'class App(Activity):\n    x = 7\n    def f(self):\n        print(self.x)\nf()\n', 'ok'),
    ('模块类', 'self.x 赋值改字段', 'class App(Activity):\n    x = 0\n    def f(self):\n        self.x = 5\n        print(self.x)\nf()\n', 'ok'),
    ('模块类', 'self.x 增强赋值', 'class App(Activity):\n    n = 0\n    def tick(self):\n        self.n += 1\n    def go(self):\n        self.tick()\n        print(self.n)\ngo()\n', 'ok'),
    ('模块类', '普通属性访问 a.b', 'x = a.b\n', 'no'),
    ('模块类', 'Activity 方法用 self 调用', 'class App(Activity):\n    def f(self):\n        print(self.g())\n    def g(self):\n        return 1\nf()\n', 'ok'),
    ('模块类', 'Activity 里漏写 global', 'class App(Activity):\n    v = 0\n    def f(self):\n        v = 2\n', 'no'),
]


def main(argv):
    if len(argv) < 2:
        print('用法: probe_syntax.py <pcc.exe> [aipy_run.exe]',
              file=sys.stderr)
        return 2
    pcc = argv[1]
    run_exe = argv[2] if len(argv) > 2 else None
    if not os.path.isfile(pcc):
        print('找不到 %s' % pcc, file=sys.stderr)
        return 2
    failures = []
    stats = {'ok': 0, 'no': 0}
    rows = []

    scratch = tempfile.mkdtemp(prefix='aipy_probe_')
    if run_exe is not None and not os.path.isfile(run_exe):
        print('找不到 %s' % run_exe, file=sys.stderr)
        return 2

    for case in CASES:
        # 4 元组是常规用例；5 元组的第 5 项是「错误信息必须包含的片段」，
        # 用来区分「语法被拒绝」与「语法接受、只是运行期缺能力」——
        # with 就属于后者：语法支持，但 manager 没有 __enter__ 才失败。
        category, name, source, expect = case[:4]
        must_contain = case[4] if len(case) > 4 else None
        ok, error = probe(pcc, source, run_exe, scratch)
        actual = 'ok' if ok else 'no'
        stats[actual] += 1
        verdict = '✓' if actual == expect else '✗'
        text_mismatch = (must_contain is not None and ok is False
                         and must_contain not in error)
        if actual != expect or text_mismatch:
            failures.append(
                '%-22s %-18s 期望 %s 实际 %s%s'
                % (category, name, expect, actual,
                   ('（%s）' % error) if error else ''))
            verdict = '✗'
        rows.append((category, name, actual, error, verdict))

    # 按类别打印，供人眼核对
    last = None
    for category, name, actual, error, verdict in rows:
        if category != last:
            print('\n── %s ──' % category)
            last = category
        mark = '支持' if actual == 'ok' else '不支持'
        detail = '' if actual == 'ok' else ('  ← %s' % error[:60])
        print('  %s %-20s %s%s' % (verdict, name, mark, detail))

    print('\n' + '=' * 70)
    print('支持 %d 条，不支持 %d 条，共 %d 条'
          % (stats['ok'], stats['no'], len(CASES)))
    if run_exe is None:
        print('（只检查了编译；传 aipy_run.exe 路径可连运行期一起验）')

    if failures:
        print('\n与预期不符（文档或编译器有问题）：', file=sys.stderr)
        for line in failures:
            print('  ' + line, file=sys.stderr)
        return 1
    print('全部与预期一致。')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
