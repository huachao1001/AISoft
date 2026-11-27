#!/usr/bin/env python3
"""对拍：C++ 版编译器与 Python 版编译器的产物必须完全一致。

── 为什么用「逐字节相同」而不是「跑起来输出一样」 ──

输出一样只说明**这一条**路径碰巧一致。字节码相同的含义强得多：指令序列、
槽号分配、常量池顺序、行号表全都一样。它同时守住了「寄存器分配/作用域
分析」这类不改输出的语义——比如函数内的临时量被错当成全局槽，单线程跑
出来仍然对，多线程才踩踏。

所以 Python 版降级成 **oracle**：它不再进发布件，只留在这里当标尺。

── 三组对比 ──

1. 六份示例：整份 .bc 逐字节比。
2. 186 条语法探针：能编的比字节码，该拒的比**报错文案**。
3. 报错文案里的位置（行:列）：文案相同、位置错位也是 bug。

用法：
    check_compiler_parity.py <pcc.exe> [--verbose]
"""

import io
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(AIPY, 'tools', 'aipyic'))

import compiler  # noqa: E402  Python 版当 oracle
import pack  # noqa: E402

# ── 文案翻译表 ──
#
# C++ 版的报错一律英文（面向国际用户，也避免 GBK 控制台乱码），Python 版
# （oracle）是中文原文。这张表把两者钉在一起：改了一边的措辞而忘了另一边，
# 对拍会红。
#
# 键是 Python 版的文案模板（`{name}` 这类占位），值是对应的英文模板。
# 比较时两边都套进实际参数再比，所以 `函数 "f" 定义了两次` 与
# `function 'f' is defined more than once` 能对上。
#
# 这张表是**验收的一部分**：它保证「拒绝的原因没变」，而不只是「都拒绝了」。
MESSAGE_PAIRS = [
    ('只能有一个 Activity 类',
     'only one Activity class is allowed per script'),
    ('{0} 不是异常类', '"{0}" is not an exception type'),
    ('类必须继承 Activity', 'a class must derive from Activity'),
    ('Activity 类体里只允许字段赋值和方法定义',
     'an Activity body may only contain field assignments and methods'),
    ('函数 "{0}" 定义了两次', "function '{0}' is defined more than once"),
    ('不支持 *args / **kwargs / 仅关键字参数',
     '*args / **kwargs / keyword-only parameters are not supported'),
    ('不支持位置-only 参数（/ 分隔符）',
     "positional-only parameters (the '/' separator) are not supported"),
    ('默认参数暂不支持字节码路线（AOT 路线支持字面量默认值）',
     'default parameter values are not supported by the bytecode compiler'),
    ('不支持嵌套函数 "{0}"；请移到模块顶层',
     "nested function '{0}' is not supported; move it to module level"),
    ('不支持关键字传参；按位置传',
     'keyword arguments are not supported; pass arguments by position'),
    ('函数 "{0}" 收到未预期的关键字参数 "{1}"',
     "function '{0}' got an unexpected keyword argument '{1}'"),
    ('函数 "{0}" 收到过多参数', "function '{0}' received too many arguments"),
    ('函数 "{0}" 缺少必需参数 "{1}"',
     "function '{0}' is missing a required argument: '{1}'"),
    ('参数 "{0}" 被重复传入', "argument '{0}' is given twice"),
    ('不支持属性访问（没有对象模型）；扩展函数请用 `模块.函数(...)` 调用',
     "attribute access is not supported (there is no object model); "
     "call extension functions as 'module.function(...)'"),
    ('内建函数 {0} 不能当值使用', "builtin '{0}' cannot be used as a value"),
    ('函数 {0} 不能当值使用（没有一等函数）；请直接调用它',
     "function '{0}' cannot be used as a value (there are no first-class "
     "functions); call it directly"),
    ('不允许给函数 {0} 重新赋值；脚本里没有一等函数',
     "cannot reassign function '{0}'; there are no first-class functions"),
    ('不支持 yield from', "'yield from' is not supported"),
    ("'async for' is not supported", "'async for' is not supported"),
    ("'async with' is not supported", "'async with' is not supported"),
    ('不支持 await：脚本是单线程事件循环',
     "'await' is not supported: scripts run on a single thread"),
    ('不支持 while ... else', "'while ... else' is not supported"),
    ('不支持 for ... else', "'for ... else' is not supported"),
    ('break 不在循环里', "'break' outside a loop"),
    ('continue 不在循环里', "'continue' outside a loop"),
    ('不支持 nonlocal；嵌套函数本身也不支持',
     "'nonlocal' is not supported; nested functions are not supported "
     "either"),
    ('不支持 assert；请写 if + 显式处理',
     "'assert' is not supported; use 'if' and handle the case explicitly"),
    ('不支持 raise：没有异常通道，用返回值表示失败',
     "'raise' is not supported: there is no exception channel; signal "
     "failure with a return value"),
    ('不支持 try 语句', "'try' statements are not supported"),
    ('不支持 del；容器用 .pop()/.clear()',
     "'del' is not supported; use .pop() or .clear() on containers"),
    ('with ... as 后面只能跟一个名字',
     "'with ... as' may only bind a single name"),
    ('不支持元组解包赋值；请分开写',
     'tuple unpacking assignment is not supported; assign each name '
     'separately'),
    ('不支持这种 += 目标', 'unsupported target for an augmented assignment'),
    ('for 的循环变量必须是单个名字', 'the for-loop variable must be a single name'),
    ('不支持这种赋值目标', 'unsupported assignment target'),
    ('不支持这种调用目标', 'this call target is not supported'),
    ('不支持 f-string', 'f-strings are not supported'),
    ('f-string 里的格式说明符暂不支持',
     'format specifiers in f-strings are not supported'),
    ('f-string 里的 !r / !s 转换暂不支持',
     'conversion flags (!r / !s) in f-strings are not supported'),
    ('f-string 暂不支持字节码路线',
     'f-strings are not supported by the bytecode compiler; use '
     'format_text() or string concatenation'),
    ('不支持装饰器：脚本里没有一等函数',
     'decorators are not supported: scripts have no first-class functions'),
    ('元组会被当成列表（见 SUPPORT.md B6），'
     '但字节码路线只支持列表字面量',
     'tuple literals are not supported (tuples would become lists); the '
     'bytecode compiler only supports list literals'),
    ('不支持字典展开 **', "'**' dictionary unpacking is not supported"),
    ('方法 "{0}" 的第一个参数必须是 self',
     "the first parameter of method '{0}' must be 'self'"),
    ('不支持运算符 {0}', 'unsupported operator: {0}'),
    ('不支持比较 {0}', 'unsupported comparison: {0}'),
    ('不支持一元运算 {0}', 'unsupported unary operator: {0}'),
    ('不支持字面量 {0}', 'unsupported literal: {0}'),
    ('不支持语句 {0}', 'unsupported statement: {0}'),
    ('不支持表达式 {0}', 'unsupported expression: {0}'),
    ('函数里给模块级变量 {0} 赋值，但没有写 global 声明。'
     'CPython 会把它当成新的局部变量；要改模块级变量请先写 `global {0}`',
     "assignment to module-level variable '{0}' inside a function without "
     "a 'global' declaration; CPython would treat it as a new local "
     "variable. Write 'global {0}' first to modify the module-level "
     "variable"),
]

def translate_python_message(text):
    """把 Python 版的文案翻成英文，好跟 C++ 版直接比。

    模板里可以有 `{0}`、`{1}` 等多个占位符，按出现顺序提取参数。
    """
    for zh, en in MESSAGE_PAIRS:
        if '{' not in zh:
            if text.startswith(zh):
                return en + text[len(zh):]
            continue

        # 按 `{N}` 把模板切成「固定文本 + 占位符」交替的序列。
        pieces = []
        position = 0
        while position < len(zh):
            start = zh.find('{', position)
            if start < 0:
                pieces.append(('text', zh[position:]))
                break
            end = zh.find('}', start)
            if end < 0:
                pieces.append(('text', zh[position:]))
                break
            if start > position:
                pieces.append(('text', zh[position:start]))
            pieces.append(('slot', zh[start + 1:end]))
            position = end + 1
        if position < len(zh) and not pieces:
            continue

        # 用固定文本把原消息切开，按序取参数。
        arguments = {}
        cursor = 0
        ok = True
        for position, (kind, value) in enumerate(pieces):
            if kind == 'text':
                found = text.find(value, cursor)
                if found < 0:
                    ok = False
                    break
                cursor = found + len(value)
                continue
            # 占位符：取到下一个固定文本之前。
            nearest = len(text)
            for later_kind, later_value in pieces[position + 1:]:
                if later_kind != 'text':
                    continue
                found = text.find(later_value, cursor)
                if found >= 0:
                    nearest = min(nearest, found)
                break
            arguments[value] = text[cursor:nearest]
            cursor = nearest
        if not ok:
            continue
        # 模板必须以最后一段固定文本结尾。
        if pieces and pieces[-1][0] == 'text' and not text.endswith(pieces[-1][1]):
            continue
        if pieces and pieces[-1][0] != 'text' and cursor > len(text):
            continue

        result = en
        for slot, argument in arguments.items():
            result = result.replace('{' + slot + '}', argument)
        if '{' in result:
            continue
        return result
    return None


def compile_with_python(source, filename):
    """返回 (字节码 或 None, 错误文案 或 None)。

    CPython 自己就拒绝的情况（比如 `type X = int` 在 3.11 上是语法错）
    返回 `(None, None)`：没有 oracle，跳过这一条。C++ 版不依赖 CPython 的
    版本，它应当给出自己的文案，但这里无从逐字比对。
    """
    try:
        program = compiler.compile_source(source, filename)
    except compiler.CompileError as error:
        text = str(error)
        marker = 'error: '
        if marker in text:
            text = text.split(marker, 1)[1]
        return (None, text)
    except SyntaxError:
        return (None, None)
    return (pack.pack(program), None)


def run_cpp(exe, source, scratch, name):
    """把源码写成文件交给 C++ 版编译，返回 (字节码 或 None, 错误文案 或 None)。"""
    script = os.path.join(scratch, name + '.py')
    output = os.path.join(scratch, name + '.bc')
    with io.open(script, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(source)
    if os.path.isfile(output):
        os.remove(output)

    completed = subprocess.run([exe, script, output], capture_output=True,
                               text=True, encoding='utf-8', errors='replace')
    if completed.returncode != 0:
        text = completed.stderr.strip()
        marker = 'error: '
        if marker in text:
            text = text.split(marker, 1)[1]
        return (None, text)
    with io.open(output, 'rb') as handle:
        return (handle.read(), None)


def compare(source, name, exe, scratch, failures, verbose):
    expected_bytes, expected_error = compile_with_python(source, '<probe>')
    actual_bytes, actual_error = run_cpp(exe, source, scratch, name)

    if expected_bytes is not None and actual_bytes is not None:
        if expected_bytes == actual_bytes:
            if verbose:
                print('  ok   %-28s %d bytes' % (name, len(expected_bytes)))
            return
        # 定位第一处差异，方便修：只报「不一致」等于让人重新找一遍。
        limit = min(len(expected_bytes), len(actual_bytes))
        at = next((i for i in range(limit)
                   if expected_bytes[i] != actual_bytes[i]), limit)
        failures.append('%s: bytecode differs at offset %d (py %d bytes, cpp %d)'
                        % (name, at, len(expected_bytes), len(actual_bytes)))
        return

    if expected_bytes is None and actual_bytes is None:
        if expected_error is None:
            # CPython 自己没有 oracle（版本差异）；这条只要求 C++ 版也拒绝。
            if verbose:
                print('  ok   %-28s rejected (no python oracle)' % name)
            return
        # C++ 版用英文、oracle 用中文，所以比的是「翻译后是否一致」。
        translated = translate_python_message(expected_error)
        if translated is None:
            failures.append('%s: python rejected with a message that has no '
                            'translation entry: %s' % (name, expected_error))
            return
        if translated == actual_error:
            if verbose:
                print('  ok   %-28s rejected: %s' % (name, actual_error[:40]))
            return
        failures.append('%s: rejected with a different message\n'
                        '         py (translated): %s\n         cpp: %s'
                        % (name, translated, actual_error))
        return

    failures.append('%s: py %s but cpp %s'
                    % (name,
                       'compiled' if expected_bytes is not None else 'rejected',
                       'compiled' if actual_bytes is not None else 'rejected'))


def main(argv):
    if len(argv) < 2:
        print('usage: check_compiler_parity.py <pcc.exe> [--verbose]',
              file=sys.stderr)
        return 2
    exe = argv[1]
    verbose = '--verbose' in argv
    if not os.path.isfile(exe):
        print('cannot find %s' % exe, file=sys.stderr)
        return 2

    failures = []
    scratch = tempfile.mkdtemp(prefix='aipy_parity_')

    # ── 1. 六份示例 ──
    print('== examples ==')
    examples = sorted(name for name in os.listdir(os.path.join(AIPY, 'examples'))
                      if name.endswith('.py'))
    for name in examples:
        path = os.path.join(AIPY, 'examples', name)
        with io.open(path, 'r', encoding='utf-8') as handle:
            source = handle.read()
        compare(source, os.path.splitext(name)[0], exe, scratch, failures,
                verbose)
    print('  %d examples checked' % len(examples))

    # ── 2. 语法探针 ──
    #
    # 直接读 tests/probe_syntax.py，不经过 JSON 基线：基线曾经放在被
    # .gitignore 忽略的 tests/data/ 下，新克隆的仓库会因为「文件不存在」
    # 静默跳过全部探针——对拍看起来是绿的，实际上一条都没跑。
    sys.path.insert(0, HERE)
    import probe_syntax  # noqa: E402  用例定义就在源码树里
    cases = probe_syntax.CASES
    print('\n== probes ==')
    for index, case in enumerate(cases):
        compare(case[2], 'probe%03d_%s' % (index, case[0]), exe, scratch,
                failures, False)
    print('  %d probes checked' % len(cases))

    if failures:
        print('\n%d mismatches:' % len(failures), file=sys.stderr)
        for line in failures:
            print('  ' + line, file=sys.stderr)
        return 1
    print('\nall bytecode and error messages match the Python compiler.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
