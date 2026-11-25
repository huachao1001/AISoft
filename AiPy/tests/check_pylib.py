#!/usr/bin/env python3
"""pylib 按需链接的端到端验证。

── 验什么 ──

设计理念第二条说：C++ 编好的静态库以 pylib 形式与 VM 链接成 exe，
且**按需**——脚本没用到的库不进产物。这件事有三处会静默出错：

1. **链了却调不到**：注册项被链接器丢弃（`const` 让它是内部链接，
   `/O2` 直接消除），症状是「链接成功、运行时报扩展没链接」。
2. **该按需却全打**：没 import 的库也进了产物，体积白涨。
3. **没链却不报错**：脚本调到不存在的扩展时静默返回 None，比崩溃更难查。

三条都必须真的造出 exe 来跑，所以这个测试会链接两次。

用法：check_pylib.py <aipyic.py> <build 目录> <lib 目录> <python>
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(AIPY, 'tools', 'aipyic'))

import bundle
import compiler
import pack

# 用到 text 模块的三个函数。
USE_TEXT = '''import text

print(text.upper("hello"))
print(text.repeat("ab", 3))
print(text.shout("hey"))
'''

EXPECTED = 'HELLO\nababab\nHEY!\n'

# 完全不用扩展。
PLAIN = '''x = 1 + 2
print(x)
'''


def build_exe(aipyic, script, output, build_dir, lib_dir, python):
    command = [python, aipyic, '--exe', script, output, '--build-dir', build_dir]
    if lib_dir:
        command += ['--lib-dir', lib_dir]
    completed = subprocess.run(command, capture_output=True, text=True,
                               encoding='utf-8')
    return completed


def run(exe):
    completed = subprocess.run([exe], capture_output=True, text=True,
                               encoding='utf-8')
    return completed.returncode, completed.stdout, completed.stderr


def main(argv):
    if len(argv) < 5:
        print('用法: check_pylib.py <aipyic.py> <build> <libdir> <python>',
              file=sys.stderr)
        return 2
    aipyic, build_dir, lib_dir, python = argv[1], argv[2], argv[3], argv[4]
    aipyic = os.path.abspath(aipyic)
    build_dir = os.path.abspath(build_dir)
    lib_dir = os.path.abspath(lib_dir)

    failures = []
    scratch = tempfile.mkdtemp(prefix='aipy_pylib_')

    # ── 1. 链了 pylib：必须能调到 ──
    script = os.path.join(scratch, 'use_text.py')
    with open(script, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(USE_TEXT)
    output = os.path.join(scratch, 'with_pylib.exe')
    result = build_exe(aipyic, script, output, build_dir, lib_dir, python)
    if result.returncode != 0:
        failures.append('打包失败：%s' % (result.stderr.strip() or result.stdout.strip()))
        print('  ✗ 链了 pylib 的打包失败')
    else:
        code, out, err = run(output)
        if code != 0 or out != EXPECTED:
            failures.append('链了 pylib 但输出不符：rc=%d out=%r err=%r'
                            % (code, out, err.strip()))
            print('  ✗ 链了 pylib 却调不到')
        else:
            print('  ✓ 链了 pylib：%s' % out.replace('\n', ' | ').strip())

    # ── 2. 不用扩展：产物里不该有 pylib 的代码 ──
    plain_script = os.path.join(scratch, 'plain.py')
    with open(plain_script, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(PLAIN)
    plain_exe = os.path.join(scratch, 'plain.exe')
    result = build_exe(aipyic, plain_script, plain_exe, build_dir, lib_dir, python)
    if result.returncode != 0:
        failures.append('不用扩展的打包失败：%s' % result.stderr.strip())
        print('  ✗ 不用扩展的打包失败')
    else:
        code, out, err = run(plain_exe)
        if code != 0 or out.strip() != '3':
            failures.append('不用扩展的产物跑不对：rc=%d out=%r' % (code, out))
            print('  ✗ 不用扩展的产物跑不对')
        else:
            blob = open(plain_exe, 'rb').read()
            marker = b'aipy_pylib_entry_text'
            if marker in blob:
                failures.append('不用扩展的产物里居然有 text_pylib 的注册符号')
                print('  ✗ 按需失效：不用扩展却把 pylib 打进去了')
            else:
                print('  ✓ 按需生效：不用扩展的产物里没有 pylib 符号')

    # ── 3. 用了扩展但没链：必须报出可读的错误，不能静默 ──
    program = compiler.compile_source(USE_TEXT, 'use_text.py')
    if program['extensions'] != ['text.upper', 'text.repeat', 'text.shout']:
        failures.append('扩展表内容不对：%r' % (program['extensions'],))
        print('  ✗ 扩展表内容不对')
    else:
        print('  ✓ 扩展表：%s' % ', '.join(program['extensions']))

    bare = os.path.join(build_dir, 'aipy_app.exe')
    if not os.path.isfile(bare):
        failures.append('找不到裸 launcher %s' % bare)
    else:
        bc = os.path.join(scratch, 'use_text.bc')
        pack.write_file(program, bc)
        broken = os.path.join(scratch, 'no_pylib.exe')
        bundle.bundle(bare, bc, broken)
        code, out, err = run(broken)
        if code == 0:
            failures.append('没链 pylib 却跑成功了（应当报错）')
            print('  ✗ 没链 pylib 却静默成功')
        elif 'text.upper' not in err:
            failures.append('报错没提到具体函数：%r' % err.strip())
            print('  ✗ 报错没提到具体函数')
        else:
            print('  ✓ 没链 pylib：%s' % err.strip())

    if failures:
        print('\n失败：', file=sys.stderr)
        for line in failures:
            print('  ' + line, file=sys.stderr)
        return 1
    print('\npylib 按需链接全部通过。')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
