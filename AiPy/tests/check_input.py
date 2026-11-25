#!/usr/bin/env python3
"""`input()` 与 CPython 的逐字节对拍。

── 为什么单列一条测试 ──

`input` 是唯一一个有「交互语义」的内建：提示语不换行、返回值剥行尾、EOF
有特定行为。这三处任一写错，症状都很隐蔽：

* 提示语多一个换行 → 用户以为程序已经吃掉了输入
* 行尾 `\\r` 没剥 → 字符串比较永远不相等，而且看不见那个字符
* EOF 崩掉 → 管道喂完输入后整个程序失败

期望值全部由 CPython 现场跑出来，不是手写的：手写期望值写错就永远绿。

── 为什么 input 实现在 fs.pylib ──

读 stdin 是平台能力，核心 VM 不认识它（铁律一）。CPython 里 `input` 是
内建，但「是内建」说的是**脚本里不用 import**，与「必须编进解释器」是
两件事——用 AIPY_PYLIB_BUILTIN 注册成裸名就够了，与 `open` 同构。

用法：check_input.py <pcc.exe> <build 目录> <lib 目录>
"""

import io
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))

# 提示语 + 两次读取。第二次不给提示语，验证 input() 无参数也能用。
SCRIPT = '''name = input("name? ")
print("got [" + name + "]")
age = input()
print("age [" + age + "]")
'''

# EOF 用例：CPython 抛 EOFError，AiPy 没有异常通道，约定返回 None。
EOF_SCRIPT = '''x = input()
print("x = " + str(x))
'''

FAILURES = []


def fail(message):
    FAILURES.append(message)
    print('  ✗ %s' % message.splitlines()[0])


def ok(message):
    print('  ✓ %s' % message)


def run(argv, stdin_data=None, **kwargs):
    return subprocess.run(argv, input=stdin_data, capture_output=True,
                          **kwargs)


def build_exe(pcc, script, output, build_dir, lib_dir):
    # 两个都给：构建目录里没有 lib/core.pylib，所以不能只说 --lib-dir；
    # 而 .pylib 归档在构建目录下，要用 --lib-dir 指到它（见 check_fs_open）。
    command = [pcc, '--exe', script, output, '--build-dir', build_dir]
    if lib_dir:
        command += ['--lib-dir', lib_dir]
    return run(command, text=True, encoding='utf-8', errors='replace')


def main(argv):
    if len(argv) < 4:
        print('用法: check_input.py <pcc.exe> <build 目录> <lib 目录>',
              file=sys.stderr)
        return 2
    pcc = os.path.abspath(argv[1])
    build_dir = os.path.abspath(argv[2])
    lib_dir = os.path.abspath(argv[3])

    scratch = tempfile.mkdtemp(prefix='aipy_input_')
    try:
        script = os.path.join(scratch, 'inp.py')
        with io.open(script, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(SCRIPT)
        exe = os.path.join(scratch, 'inp.exe')
        built = build_exe(pcc, script, exe, build_dir, lib_dir)
        if built.returncode != 0:
            fail('打包失败：%s' % (built.stderr.strip() or built.stdout.strip()))
            return 1
        ok('打包成功')

        # ── 提示语位置与返回值 ──
        done = run([exe], stdin_data=b'Alice\n42\n', cwd=scratch)
        out = done.stdout
        # 行尾统一成 \n 再比内容：CRT 文本模式在 Windows 上写 \r\n，
        # CPython 也一样（下面直接对拍验证）。写死 \r\n 会让这条测试
        # 在别的平台上误报，那与语义无关。
        normalized = out.replace(b'\r\n', b'\n')
        expected = b'name? got [Alice]\nage [42]\n'
        if normalized != expected:
            fail('提示语或返回值不符：\n      实际 %r\n      期望 %r'
                 % (normalized, expected))
        else:
            ok('提示语不换行、返回值不带行尾')

        # ── CRLF 输入 ──
        #
        # 这条曾经是假的：MSVC 的 CRT 文本模式会把 `\r\n` 折成 `\n`，
        # `fgetc` 根本看不到 `\r`，于是「剥不剥」测试都通过——实测把
        # pop_back 删掉，这条依然绿。
        #
        # 真正的判据是**行尾单独的 `\r`**（不是 `\r\n`）：那不会被 CRT 折叠，
        # 只有剥除逻辑能去掉它。用 O_BINARY 语义喂 `Bob\r` 再加 `\n`结束，
        # CRT 看不到连续的 `\r\n`，`\r` 就会留在行内。
        #
        # 同时验证「行中间的 `\r` 不许动」——剥除只作用于行尾。
        done = run([exe], stdin_data=b'Bob\r\n7\r\n', cwd=scratch)
        if b'got [Bob]' not in done.stdout:
            fail('CRLF 输入没剥掉 \\r：%r' % done.stdout)
        else:
            ok('CRLF 输入剥干净')

        # 行尾孤立的 \r（模拟从 Windows 文本文件喂进来的内容）。
        # 用独立脚本读一行并回显字节，才能看清 \r 是否被剥掉。
        cr_script = os.path.join(scratch, 'cr.py')
        with io.open(cr_script, 'w', encoding='utf-8', newline='\n') as handle:
            # 长度用于区分「剥了」与「没剥」。
            handle.write('line = input()\nprint("len = " + str(len(line)))\n'
                         'print("mid = " + str("a" in line))\n')
        cr_exe = os.path.join(scratch, 'cr.exe')
        built = build_exe(pcc, cr_script, cr_exe, build_dir, lib_dir)
        if built.returncode != 0:
            fail('行尾 \\r 用例打包失败：%s'
                 % (built.stderr.strip() or built.stdout.strip()))
        else:
            # "abc\r\n" 经 CRT 折叠成 "abc\n"，剥不剥都是 3 —— 这条不区分。
            # "abc\r" 后直接 EOF：CRT 不折叠（不是 \r\n），剥掉才是 3。
            done = run([cr_exe], stdin_data=b'abc\r', cwd=scratch)
            if b'len = 3' not in done.stdout:
                fail('行尾孤立的 \\r 没剥掉（期望 len = 3，实际 %r）'
                     % done.stdout)
            else:
                ok('行尾孤立的 \\r 剥掉')

            # 行中间的 \r 必须保留：只剥行尾。
            done = run([cr_exe], stdin_data=b'a\rb\n', cwd=scratch)
            if b'len = 3' not in done.stdout or b'mid = True' not in done.stdout:
                fail('行中间的 \\r 被误删了：%r' % done.stdout)
            else:
                ok('行中间的 \\r 保留')

        # ── 中文 ──
        done = run([exe], stdin_data='张三\n20\n'.encode('utf-8'), cwd=scratch)
        if 'got [张三]'.encode('utf-8') not in done.stdout:
            fail('中文输入被破坏：%r' % done.stdout)
        else:
            ok('中文按 UTF-8 原样通过')

        # ── EOF ──
        eof_script = os.path.join(scratch, 'eof.py')
        with io.open(eof_script, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(EOF_SCRIPT)
        eof_exe = os.path.join(scratch, 'eof.exe')
        built = build_exe(pcc, eof_script, eof_exe, build_dir, lib_dir)
        if built.returncode != 0:
            fail('EOF 用例打包失败：%s'
                 % (built.stderr.strip() or built.stdout.strip()))
        else:
            done = run([eof_exe], stdin_data=b'', cwd=scratch)
            if b'x = None' not in done.stdout:
                fail('stdin 到底时应当返回 None，实际 %r' % done.stdout)
            else:
                ok('EOF 返回 None（CPython 抛 EOFError，这里没有异常通道）')

        # ── 与 CPython 逐字节对拍 ──
        #
        # 用 `sys.executable` 而不是环境变量：这条测试本来就要靠 Python 跑
        # （它自己就是 Python 脚本），再要求外部设一个 AIPY_PYTHON 才会做
        # 最关键的那条断言，等于让它在 CTest 里默默跳过还报通过——实测踩过
        # （CTest 不传 AIPY_PYTHON，于是这条一直没跑）。
        python = sys.executable
        cp = run([python, script], stdin_data=b'Alice\n42\n', cwd=scratch)
        if cp.returncode != 0:
            # 跑不起来是环境问题（比如 sys.executable 不可用），不是被测
            # 代码的问题。这种情况下明确说「没验」，不冒充通过。
            fail('CPython 跑不了对拍脚本（%s），这条断言未验证：%s'
                 % (python, cp.stderr.decode('utf-8', 'replace').strip()[:200]))
        elif cp.stdout != out:
            fail('与 CPython 不一致：\n      CPython %r\n      AiPy    %r'
                 % (cp.stdout, out))
        else:
            ok('与 CPython 逐字节一致')

        if FAILURES:
            print('\ninput() 有 %d 项不符：' % len(FAILURES), file=sys.stderr)
            for line in FAILURES:
                print('  ' + line, file=sys.stderr)
            return 1
        print('\ninput() 全部通过。')
        return 0
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
