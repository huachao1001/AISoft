#!/usr/bin/env python3
# 检查示例脚本是不是「合法的、符合 pyuic 约束的 Python」
#
# ── 为什么需要这个 ──
#
# 光看 pyuic 能生成 C++ 不够：生成器可能就是错的，而错的生成器会把
# 错的脚本编成一堆「看起来正常」的 C++。真正要证明的是：
#
#   同一份脚本，CPython 跑一遍、编译产物跑一遍，结果一样。
#
# 这一条没法自动验证全部行为（那需要把 pyui 控件也模拟出来），但最容易
# 出错的语义——函数级作用域与 global——可以在这里逐字验证：编译前先用
# CPython 的 symtable 模块比对「pyuic 认为的局部量」与「CPython 认为的
# 局部量」。两者不一致就说明生成代码会与解释器行为不同。
#
# 用法：
#   python tools/pyuic/check_scope.py examples/calculator/calculator.py
import ast
import sys
import symtable


def cpython_locals(source):
    """CPython 眼中的每个函数的局部变量集合。"""
    table = symtable.symtable(source, '<check>', 'exec')
    result = {}
    for child in table.get_children():
        if child.get_type() != 'function':
            continue
        result[child.get_name()] = {
            symbol.get_name()
            for symbol in child.get_symbols()
            if symbol.is_local() and not symbol.is_parameter()
        }
    return result


def pyuic_locals(source):
    """pyuic 眼中的每个函数的局部变量集合（与生成器同一套规则）。

    规则：函数体里被赋值的名字，减去参数，减去 global 声明的名字，
    剩下的就是局部量。这与 Generator.emit_function 完全一致——
    故意重复这一小段，是要让两边**各自独立**算一次再比对。
    """
    tree = ast.parse(source)
    result = {}
    for node in tree.body:
        if not isinstance(node, ast.FunctionDef):
            continue
        params = {arg.arg for arg in node.args.args}

        declared_global = set()
        for statement in node.body:
            if isinstance(statement, ast.Global):
                declared_global.update(statement.names)

        assigned = set()
        for child in ast.walk(node):
            if isinstance(child, ast.Assign):
                for target in child.targets:
                    if isinstance(target, ast.Name):
                        assigned.add(target.id)
            elif isinstance(child, (ast.AnnAssign, ast.AugAssign)):
                if isinstance(child.target, ast.Name):
                    assigned.add(child.target.id)
            elif isinstance(child, ast.For):
                if isinstance(child.target, ast.Name):
                    assigned.add(child.target.id)

        result[node.name] = assigned - params - declared_global
    return result


def main(paths):
    failures = 0
    for path in paths:
        with open(path, 'r', encoding='utf-8-sig') as handle:
            source = handle.read()
        try:
            expected = cpython_locals(source)
            actual = pyuic_locals(source)
        except SyntaxError as error:
            print('%s:%s:%s: error: %s' % (path, error.lineno, error.offset, error.msg))
            failures += 1
            continue

        for name in sorted(expected):
            if expected[name] != actual.get(name, set()):
                print('%s: %s() 的局部量不一致' % (path, name))
                print('  CPython: %s' % sorted(expected[name]))
                print('  pyuic:   %s' % sorted(actual.get(name, set())))
                failures += 1

    if failures:
        print('scope check: %d 个不一致' % failures)
        return 1
    print('scope check OK: %d 个脚本与 CPython 一致' % len(paths))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
