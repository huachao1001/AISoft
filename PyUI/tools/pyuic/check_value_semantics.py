#!/usr/bin/env python3
# 把 tests/test_py_value.cpp 里的语义断言拿到 CPython 里跑一遍
#
# ── 为什么不用现成的单元测试框架 ──
#
# 要证明的不是「C++ 实现内部自洽」，而是「C++ 实现与 CPython 一致」。
# 所以这里的基准必须是真正的 CPython，不能是另一份人写的期望值。
#
# 做法：本文件里逐条照抄 test_py_value.cpp 的用例（同样的断言、同样的值），
# 直接交给 CPython 求值。跑通就说明那份 C++ 测试里的**期望值**没写错；
# 再接上 C++ 侧的实际值，两边一对照就是差分测试。
#
# 用法：
#   python tools/pyuic/check_value_semantics.py
import sys

failures = 0


def check(label, actual, expected):
    global failures
    if actual != expected:
        print('  FAIL %s: 期望 %r，实际 %r' % (label, expected, actual))
        failures += 1


def main():
    # ── 真值 ──
    check('bool(None)', bool(None), False)
    check('bool(0)', bool(0), False)
    check('bool(1)', bool(1), True)
    check('bool(-1)', bool(-1), True)
    check('bool([])', bool([]), False)
    check('bool([0])', bool([0]), True)
    check('bool("")', bool(""), False)
    check('bool("0")', bool("0"), True)

    # ── 整数与浮点 ──
    check('7 / 2', 7 / 2, 3.5)
    check('-7 // 2', -7 // 2, -4)
    check('7 // 2', 7 // 2, 3)
    check('-7 % 3', -7 % 3, 2)
    check('7 % 3', 7 % 3, 1)
    check('type(1 + 2.5)', type(1 + 2.5), float)
    check('1 + 2.5', 1 + 2.5, 3.5)
    check('"a" + "b"', "a" + "b", "ab")
    check('"ab" * 3', "ab" * 3, "ababab")
    check('2 ** 10', 2 ** 10, 1024)
    check('2 ** -1', 2 ** -1, 0.5)
    # 幂的类型：整数底数 + 非负整数指数 → int。返回 float 会让
    # str(2 ** 10) 变成 "1024.0"
    check('type(2 ** 10)', type(2 ** 10), int)
    check('str(2 ** 10)', str(2 ** 10), '1024')
    check('type(4 ** 0.5)', type(4 ** 0.5), float)
    check('0 ** 0', 0 ** 0, 1)

    # ── 比较 ──
    check('1 == 1.0', 1 == 1.0, True)
    check('0 == False', 0 == False, True)  # noqa: E712 —— 就是要测这个
    check('True == 1', True == 1, True)
    check('"a" < "b"', "a" < "b", True)
    check('"b" >= "b"', "b" >= "b", True)
    check('2 in [1, 2]', 2 in [1, 2], True)
    check('3 in [1, 2]', 3 in [1, 2], False)
    check('"23" in "123"', "23" in "123", True)
    check('True + True', True + True, 2)

    # ── 逻辑：返回操作数本身 ──
    check('"" or "默认"', "" or "默认", "默认")
    check('"a" and "b"', "a" and "b", "b")

    # ── 列表 ──
    a = [1]
    b = a
    b.append(2)
    check('b = a; b.append(2); len(a)', len(a), 2)
    check('[1,2,3][-1]', [1, 2, 3][-1], 3)
    check('[0,1,2,3][1:3]', [0, 1, 2, 3][1:3], [1, 2])
    original = [0, 1, 2, 3]
    original[1:3]
    check('切片不改原列表', len(original), 4)
    items = [1, 2]
    removed = items.pop()
    check('pop()', removed, 2)
    check('pop() 之后长度', len(items), 1)
    numbers = [3, 1, 2]
    numbers.sort()
    check('sort()', numbers, [1, 2, 3])

    # ── 字典 ──
    d = {'k': 7}
    check('d["k"]', d['k'], 7)
    check('d.get("nope")', d.get('nope'), None)
    check('d.get("nope", 0)', d.get('nope', 0), 0)
    ordered = {}
    ordered['b'] = 1
    ordered['a'] = 2
    ordered['c'] = 3
    check('字典插入顺序', list(ordered.keys()), ['b', 'a', 'c'])
    d2 = {'k': 5}
    popped = d2.pop('k')
    check('dict.pop()', popped, 5)
    check('dict.pop() 之后', d2.get('k'), None)
    da = {}
    db = da
    db['k'] = 1
    check('字典引用语义', da['k'], 1)

    # ── 字符串 ──
    check('"abc"[1]', "abc"[1], 'b')
    check('"中文"[1]', "中文"[1], '文')
    check('len("中文")', len("中文"), 2)
    check('"中文abc"[0:2]', "中文abc"[0:2], '中文')
    check('"中文abc"[2:]', "中文abc"[2:], 'abc')

    # ── 数值转换 ──
    check('int(-1.7)', int(-1.7), -1)
    check('int(1.9)', int(1.9), 1)
    check('str(1.0)', str(1.0), '1.0')
    check('str(0.5)', str(0.5), '0.5')
    check('str(1)', str(1), '1')
    check('str(1.0)', str(1.0), '1.0')
    check('str(-0.0)', str(-0.0), '-0.0')

    # 最短往返表示：这几个值是 %.10g 之类的「看起来干净」写法会写错的
    check('str(0.1 + 0.2)', str(0.1 + 0.2), '0.30000000000000004')
    check('str(1 / 3)', str(1 / 3), '0.3333333333333333')
    check('str(0.1)', str(0.1), '0.1')

    # 指数记法的阈值：decpt <= -4 或 decpt > 16
    check('str(1e15)', str(1e15), '1000000000000000.0')
    check('str(1e16)', str(1e16), '1e+16')
    check('str(0.0001)', str(0.0001), '0.0001')
    check('str(1e-05)', str(1e-05), '1e-05')

    check('round(0.5)', round(0.5), 0)
    check('round(1.5)', round(1.5), 2)
    check('round(2.5)', round(2.5), 2)
    check('round(3.5)', round(3.5), 4)
    check('round(-0.5)', round(-0.5), 0)
    check('round(-1.5)', round(-1.5), -2)
    check('round(3.14159, 2)', round(3.14159, 2), 3.14)

    # round(x, n) 的经典翻车点：先乘再取整会错，必须对精确十进制值舍入
    check('round(2.675, 2)', round(2.675, 2), 2.67)
    check('round(1.335, 2)', round(1.335, 2), 1.33)
    check('round(8.005, 2)', round(8.005, 2), 8.01)
    check('round(4.35, 1)', round(4.35, 1), 4.3)
    check('round(4.45, 1)', round(4.45, 1), 4.5)
    check('round(0.125, 2)', round(0.125, 2), 0.12)
    check('round(0.135, 2)', round(0.135, 2), 0.14)
    check('round(1.005, 2)', round(1.005, 2), 1.0)

    # ── 浅拷贝 ──
    src = [1]
    dst = list(src)
    dst.append(2)
    check('list() 是浅拷贝', len(src), 1)
    check('list() 结果长度', len(dst), 2)

    # ── None 判断 ──
    # 用变量而不是字面量：写 `1 is None` 会触发 SyntaxWarning，
    # 而这里要测的正是「非 None 的东西 is None 为假」这条规则。
    one = 1
    empty = ""
    check('None is None', None is None, True)
    check('1 is None', one is None, False)
    check('"" is None', empty is None, False)

    if failures:
        print('value semantics: %d 条与 CPython 不一致' % failures)
        return 1
    print('value semantics OK: 全部断言已与 CPython 对齐')
    return 0


if __name__ == '__main__':
    sys.exit(main())
