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
