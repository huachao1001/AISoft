#!/usr/bin/env python3
"""对拍：同一份脚本，CPython 跑一遍、AiPy VM 跑一遍，逐变量比较。

── 为什么这条测试比 C++ 的单测更重要 ──

C++ 测试里的期望值是我手写的。手写就会写错，而写错的测试**永远绿**，
它保护不了任何东西。这里的事实来源是 CPython 自己的执行结果——脚本改动
之后期望值自动跟着变，不会出现「代码对了、测试过期了」。

做法：CPython 直接 exec 脚本拿全局变量；AiPy 侧把同样的脚本编成字节码，
用 aipy_vm_dump 跑出来打印同样的格式，两边比字符串。
"""

import io
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))
AIPYIC = os.path.normpath(os.path.join(AIPY, 'tools', 'aipyic'))
sys.path.insert(0, AIPYIC)

import pack as bytecode_pack
import compiler as vm_compiler

# 只比这几类值。容器与字典要「归一化后」比，因为 CPython 的 dict_keys
# 之类的视图类型在 AiPy 里是列表——这是 SUPPORT.md B6 写明的差异。
COMPARABLE = (int, float, str, bool, list, dict)


def normalize(value):
    if isinstance(value, dict):
        return {str(k): normalize(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [normalize(v) for v in value]
    if hasattr(value, '__iter__') and not isinstance(value, (str, bytes)):
        # dict_keys / dict_values / range 之类：按列表比
        try:
            return [normalize(v) for v in value]
        except TypeError:
            return repr(value)
    return value


def cpython_globals(script):
    """CPython 执行脚本，返回可比对的全局变量。"""
    scope = {}
    with io.open(script, 'r', encoding='utf-8') as handle:
        source = handle.read()
    exec(compile(source, script, 'exec'), scope)
    out = {}
    for name, value in scope.items():
        if name.startswith('_'):
            continue
        if callable(value) or isinstance(value, type(sys)):
            continue
        if isinstance(value, COMPARABLE) or hasattr(value, '__iter__'):
            out[name] = normalize(value)
    return out


def run_vm(script, dump_exe, out_dir):
    """编译后交给 C++ 侧跑，读回它打印的 JSON。"""
    program = vm_compiler.compile_file(script)
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(script))[0]
    path = os.path.join(out_dir, stem + '.bc')
    bytecode_pack.write_file(program, path)

    completed = subprocess.run([dump_exe, path], capture_output=True, text=True,
                               encoding='utf-8')
    if completed.returncode != 0:
        raise SystemExit('VM 执行失败：%s' % (completed.stderr or completed.stdout))
    return json.loads(completed.stdout)['globals'], program


def compare(script, dump_exe, out_dir):
    expected = cpython_globals(script)
    actual, program = run_vm(script, dump_exe, out_dir)
    problems = []
    for name, want in sorted(expected.items()):
        # 只比脚本真正用到的全局变量（vm 的产物里有名字表）。
        if name not in program['globals']:
            continue
        got = actual.get(name)
        if got != want:
            problems.append('  %-18s CPython=%-20r AiPy=%r' % (name, want, got))
    return problems, len([n for n in expected if n in program['globals']])


def main(argv):
    if len(argv) < 2:
        print('用法: check_vm_semantics.py <aipy_vm_dump.exe> [script.py...]',
              file=sys.stderr)
        return 2
    dump_exe = argv[1]
    scripts = argv[2:]
    if not scripts:
        scripts_dir = os.path.join(HERE, 'scripts')
        scripts = [os.path.join(scripts_dir, name)
                   for name in sorted(os.listdir(scripts_dir))
                   if name.endswith('.py') and name != 'error_line.py']

    # 字节码写在临时目录：这份产物每次都要重新生成，留在源码树里只会
    # 变成「改了脚本但测试跑的是旧的」。
    out_dir = os.path.join(tempfile.gettempdir(), 'aipy_semantics')
    failures = 0
    for script in scripts:
        problems, compared = compare(script, dump_exe, out_dir)
        name = os.path.basename(script)
        if problems:
            failures += 1
            print('FAIL %s' % name, file=sys.stderr)
            for line in problems:
                print(line, file=sys.stderr)
        else:
            print('ok   %s（%d 个变量与 CPython 一致）' % (name, compared))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
