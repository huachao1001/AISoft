#!/usr/bin/env python3
# 语法覆盖检查：证明「pyuic 认的语法」与「CPython 认的语法」是同一套。
#
# ── 为什么需要这个 ──
#
# pyuic 的前端是 CPython 的 ast，所以**解析**这一层天然与 CPython 一致：
# 只要能在本地 Python 上 parse 的写法，pyuic 一定能拿到 AST。真正会出错的
# 是第二层——把 AST 变成 C++ 的那一步。这一步可能有两种病：
#
#   1. 该报错的没报错：语法支持不了，却生成了一段**看起来对、算出来错**的
#      C++。这类 bug 最贵，因为编译全绿、示例也跑得起来。
#   2. 能支持的却报错：明明可以机械翻译，却告诉用户不支持，等于自己砍掉
#      可用面。
#
# 所以这里为每一类语法写明**期望结局**：GENERATED（能翻译）或 ERROR
# （构建期报错）。任何一条与期望不符就失败。期望值是手工审过的——加新语法
# 时，先想清楚它属于 A 类（可机械翻译）还是 C 类（必须报错），再写在这里。
#
# 用法：
#   python tools/pyuic/check_syntax.py
import ast
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pyuic  # noqa: E402


# 每个用例：(名字, 源码, 期望, 说明)
# 期望 'ok' = 应当生成代码；'error' = 应当构建期报错。
CASES = [
    # ── A 类：能机械翻译，必须生成 ──
    ('assign', 'x = 1\ny = x + 1\n', 'ok', '模块级赋值与算术'),
    ('augassign', 'x = 1\nx += 2\n', 'ok', '增强赋值'),
    ('multi_assign', 'a = b = 1\n', 'ok', '链式赋值'),
    ('fstring_plain', 'def f(v):\n  return f"值={v}"\n', 'ok', '无格式说明的 f-string'),
    ('fstring_str_conv', 'def f(v):\n  return f"{v!s}"\n', 'ok', '!s 与默认行为等价'),
    ('default_arg', 'def f(a, b=2):\n  return a + b\n', 'ok', '字面量默认参数'),
    ('default_arg_call', 'def f(a, b=2):\n  return a + b\ndef g():\n  return f(1)\n',
     'ok', '默认参数在调用点展开'),
    ('posonly', 'def f(a, /, b):\n  return a + b\n', 'ok', '仅位置参数'),
    ('annassign', 'def f():\n  x: int = 1\n  return x\n', 'ok', '带注解的赋值'),
    ('elif_chain', 'def f(v):\n  if v == 1:\n    return 1\n  elif v == 2:\n'
                   '    return 2\n  else:\n    return 0\n', 'ok', 'elif 链'),
    ('chained_compare', 'def f(a, b, c):\n  return a < b < c\n', 'ok', '链式比较'),
    ('ternary', 'def f(v):\n  return 1 if v else 0\n', 'ok', '条件表达式'),
    ('while_break', 'def f(n):\n  i = 0\n  while i < n:\n    i += 1\n'
                    '    if i > 3:\n      break\n  return i\n', 'ok', 'while + break'),
    ('for_range', 'def f(n):\n  t = 0\n  for i in range(n):\n    t += i\n'
                  '  return t\n', 'ok', 'for + range'),
    ('for_list', 'def f(items):\n  t = 0\n  for v in items:\n    t += v\n'
                 '  return t\n', 'ok', 'for 遍历容器'),
    ('dict_literal', 'd = {"a": 1, "b": 2}\n', 'ok', '字典字面量'),
    ('slice', 'def f(items):\n  return items[1:3]\n', 'ok', '切片读取'),
    ('subscript_assign', 'd = {}\ndef f():\n  d["k"] = 1\n', 'ok', '下标赋值'),
    ('string_method', 'def f(v):\n  return v.strip().upper()\n', 'ok', '字符串方法链'),
    ('list_method', 'l = []\ndef f():\n  l.append(1)\n  l.sort()\n', 'ok', '列表就地修改'),
    ('star_args_forward', 'def g(a, b):\n  return a\n'
                          'def f(v):\n  return g(*v)\n', 'error', '*展开目前不支持'),
    ('global_ok', 'x = 1\ndef f():\n  global x\n  x = 2\n', 'ok', 'global 改写模块级'),
    ('nested_block_assign', 'def f(v):\n  if v:\n    n = 1\n  else:\n    n = 2\n'
                            '  return n\n', 'ok', '分支内赋值在函数级可见'),
    ('unused_param', 'def f(a, b):\n  return a\n', 'ok', '未使用参数不报错'),

    # ── Activity：对齐 Android 的入口写法 ──
    ('activity_basic',
     'class Main(Activity):\n  def onCreate(self):\n    set_text(find("a"), "x")\n',
     'ok', 'Activity + onCreate'),
    ('activity_field',
     'class Main(Activity):\n  total = 0\n  def onCreate(self):\n    self.total = 1\n',
     'ok', '类体字段与 self.字段'),
    ('activity_field_read',
     'class Main(Activity):\n  total = 0\n  def bump(self):\n    self.total += 1\n'
     '  def onCreate(self):\n    self.bump()\n', 'ok', 'self.方法() 与 self.x += 1'),
    ('activity_self_member',
     'class Main(Activity):\n  def onCreate(self):\n    self.ready = False\n',
     'ok', '方法体里首次出现的 self.字段也算成员'),
    ('activity_module_func',
     'def helper(v):\n  return v\nclass Main(Activity):\n'
     '  def onCreate(self):\n    helper(1)\n', 'ok', 'Activity 方法可调模块级函数'),
    ('activity_main_multiple_methods',
     'class Main(Activity):\n  def onCreate(self):\n    self.a()\n  def a(self):\n    pass\n',
     'ok', '多个方法互相调用'),

    # ── C 类：必须构建期报错（静默翻译会算错） ──
    ('decorator', '@staticmethod\ndef f():\n  return 1\n', 'error', '装饰器会替换函数对象'),
    ('global_missing', 'x = 1\ndef f():\n  x = 2\n', 'error',
     '不写 global 就是新局部量，几乎总是笔误'),
    ('fstring_format', 'def f(v):\n  return f"{v:>4}"\n', 'error', '格式说明需要迷你语言'),
    ('fstring_repr', 'def f(v):\n  return f"{v!r}"\n', 'error', '!r 需要 repr 语义'),
    ('kwonly', 'def f(*, a):\n  return a\n', 'error', '仅关键字参数'),
    ('varargs', 'def f(*args):\n  return 1\n', 'error', '*args'),
    ('kwargs', 'def f(**kw):\n  return 1\n', 'error', '**kwargs'),
    ('keyword_call', 'def f(a):\n  return a\ndef g():\n  return f(a=1)\n', 'error',
     '关键字传参'),
    ('nested_func', 'def f():\n  def g():\n    return 1\n  return g()\n', 'error',
     '嵌套函数'),
    ('lambda', 'def f():\n  return lambda: 1\n', 'error', 'lambda'),
    ('list_comp', 'def f(v):\n  return [x for x in v]\n', 'error', '列表推导'),
    ('dict_comp', 'def f(v):\n  return {x: 1 for x in v}\n', 'error', '字典推导'),
    ('set_literal', 'def f():\n  return {1, 2}\n', 'error', '集合字面量'),
    ('walrus', 'def f(v):\n  if (n := v):\n    return n\n  return 0\n', 'error',
     '海象运算符'),
    ('tuple_unpack', 'def f(v):\n  a, b = v\n  return a\n', 'error', '元组解包'),
    # 没有扩展索引时 import 必须报错：静默忽略一个 import 会让程序启动后
    # 才在调用处崩，那时已经看不出是构建期漏了 --ext-dir。
    ('import_no_index', 'import os\n', 'error', '没有扩展模块'),
    ('class_def', 'class A:\n  pass\n', 'error', '必须继承 Activity'),
    ('class_wrong_base', 'class A(Object):\n  pass\n', 'error', '基类必须是 Activity'),
    ('class_two', 'class A(Activity):\n  pass\nclass B(Activity):\n  pass\n', 'error',
     '一个进程只有一个入口界面'),
    ('class_no_self',
     'class A(Activity):\n  def onCreate():\n    pass\n', 'error',
     '方法第一个参数必须是 self'),
    ('class_self_value',
     'class A(Activity):\n  def onCreate(self):\n    x = self\n', 'error',
     'self 不能当值用'),
    ('class_unknown_field',
     'class A(Activity):\n  def onCreate(self):\n    return self.missing\n', 'error',
     'self.未知字段'),
    ('class_unknown_method',
     'class A(Activity):\n  def onCreate(self):\n    self.missing()\n', 'error',
     'self.未知方法'),
    ('class_meta_in_body',
     'class A(Activity):\n  WIDTH = 100\n', 'error', '元数据只能写在模块顶层'),
    ('class_nested_def',
     'class A(Activity):\n  def onCreate(self):\n    def g():\n      pass\n', 'error',
     '嵌套函数'),
    ('class_if_in_body',
     'class A(Activity):\n  if True:\n    pass\n', 'error',
     '类体只允许 def 与字段赋值'),
    ('try_stmt', 'def f():\n  try:\n    pass\n  except:\n    pass\n', 'error', '没有异常通道'),
    ('with_stmt', 'def f():\n  with x:\n    pass\n', 'error', '没有上下文管理器'),
    ('raise_stmt', 'def f():\n  raise ValueError()\n', 'error', '没有异常通道'),
    ('assert_stmt', 'def f():\n  assert 1\n', 'error', 'assert 用 if 代替'),
    ('delete_stmt', 'def f(l):\n  del l[0]\n', 'error', 'del'),
    ('while_else', 'def f():\n  while False:\n    pass\n  else:\n    pass\n', 'error',
     'while ... else'),
    ('for_else', 'def f(l):\n  for x in l:\n    pass\n  else:\n    pass\n', 'error',
     'for ... else'),
    ('nonlocal', 'def f():\n  def g():\n    nonlocal x\n', 'error', 'nonlocal'),
    ('bytes_literal', 'b = b"x"\n', 'error', 'bytes 字面量'),
    ('matmul', 'def f(a, b):\n  return a @ b\n', 'error', '矩阵乘法'),
    ('yield_stmt', 'def f():\n  yield 1\n', 'error', '生成器'),
]

# match 只在 3.10+ 能 parse。不放进 CASES 是因为「CPython 都 parse 不了」
# 属于语法层，与 pyuic 无关；单独探测，且在旧解释器上算通过。
MATCH_CASE = ('match_stmt', 'def f(v):\n  match v:\n    case 1:\n      return 2\n'
                            '  return 0\n')


def generate(source):
    """返回 (ok, 消息)。ok=True 表示生成了代码。"""
    tree = ast.parse(source)
    generator = pyuic.Generator('ProbeScript', '<probe>')
    try:
        generator.generate(tree)
    except pyuic.CompileError as error:
        return False, '%d:%d %s' % (error.line, error.column, error.message)
    return True, ''


def run():
    failures = 0
    cases = list(CASES)
    # 语法层探测：老解释器没有 match，直接跳过这一条
    try:
        ast.parse(MATCH_CASE[1])
        cases.append(MATCH_CASE + ('仅 3.10+ 才有此语法',))
    except SyntaxError:
        pass

    for case in cases:
        name, source, expected = case[0], case[1], case[2]
        try:
            ok, message = generate(source)
        except SyntaxError as error:
            print('FAIL %-20s CPython 自己都 parse 不了：%s' % (name, error.msg))
            failures += 1
            continue
        except Exception as error:  # 生成器崩溃比「判错」更严重
            print('FAIL %-20s 生成器崩溃 %s: %s'
                  % (name, type(error).__name__, error))
            failures += 1
            continue

        want_ok = expected == 'ok'
        if ok != want_ok:
            if want_ok:
                print('FAIL %-20s 期望能翻译，却报错：%s' % (name, message))
            else:
                print('FAIL %-20s 期望构建期报错，却静默生成了代码' % name)
            failures += 1
        elif not ok and not message:
            # 报错但没有位置：用户没法定位，按失败算
            print('FAIL %-20s 报错了但没有 file:line:col' % name)
            failures += 1

    if failures:
        print('syntax check: %d 个用例与期望不符（共 %d 个）'
              % (failures, len(cases)))
        return 1

    # ── 入口生成：Windows 必须是 wWinMain ──
    #
    # 这条不能靠「编译过」来守：int main 照样能编过，只是链接成控制台子系统，
    # 双击启动时先弹一个黑框。症状与「程序启动慢」极像，很难归因到入口。
    entry_failures = 0
    windows_entry = pyuic.main_source('ProbeScript', 'probe', windows=True)
    for needle, why in (('wWinMain', 'Windows 入口必须是 wWinMain'),
                        ('py::run(*script)', 'Windows 入口不接 char** 参数')):
        if needle not in windows_entry:
            print('FAIL %-20s %s' % ('entry_windows', why))
            entry_failures += 1
    if 'int main(' in windows_entry:
        print('FAIL %-20s Windows 入口不能再用 int main（会连成控制台程序）'
              % 'entry_windows')
        entry_failures += 1

    posix_entry = pyuic.main_source('ProbeScript', 'probe', windows=False)
    if 'int main(int argc, char** argv)' not in posix_entry:
        print('FAIL %-20s 非 Windows 入口仍然要 main(argc, argv)' % 'entry_posix')
        entry_failures += 1

    if entry_failures:
        print('syntax check: 入口生成 %d 处不符' % entry_failures)
        return 1
    total = len(cases) + 2

    ok_count = sum(1 for c in cases if c[2] == 'ok')
    print('syntax check OK: %d 个用例（%d 个生成、%d 个按预期报错）'
          % (total, ok_count, len(cases) - ok_count + 2))
    return 0


if __name__ == '__main__':
    sys.exit(run())
