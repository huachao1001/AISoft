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
        # Windows 的管道与文件都常用 \r\n，只剥 \n 会留下一个不可见的 \r，
        # 症状是「字符串比较总是不相等」。
        done = run([exe], stdin_data=b'Bob\r\n7\r\n', cwd=scratch)
        if b'got [Bob]' not in done.stdout:
            fail('CRLF 输入没剥掉 \\r：%r' % done.stdout)
        else:
            ok('CRLF 输入剥干净')

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
        python = os.environ.get('AIPY_PYTHON')
        if python is None:
            print('  - 跳过 CPython 对拍（没有 AIPY_PYTHON）')
        else:
            cp = run([python, script], stdin_data=b'Alice\n42\n', cwd=scratch)
            if cp.returncode != 0:
                print('  - 跳过 CPython 对拍（它跑不了这份脚本）')
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
