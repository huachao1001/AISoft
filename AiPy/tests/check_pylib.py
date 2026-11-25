#!/usr/bin/env python3
"""pylib 按需链接的端到端验证。

── 验什么 ──

设计理念第二条说：C++ 编好的静态库以 pylib 形式与 VM 链接成 exe，
且**按需**——脚本没用到的库不进产物。这件事有三处会静默出错：

1. **链了却调不到**：注册项被链接器丢弃（`const` 让它是内部链接，
   `/O2` 直接消除），症状是「链接成功、运行时报扩展没链接」。
2. **该按需却全打**：没 import 的库也进了产物，体积白涨。
3. **没链却不报错**：脚本调到不存在的扩展时静默返回 None，比崩溃更难查。

三条都必须真的造出 exe 来跑，所以这个测试会链接多次。

── 为什么用 --linked-info 而不是打包摘要 ──

`pcc --exe` 会打印 `linked functions: ...`，但那是打包器**自己说**它
点了哪些名，不是链接器实际收下的。曾经为了验证测试本身而故意注入「全点名」，
摘要照旧只列脚本用到的函数、测试依旧报通过——测试信了被告，没信证据。

`--linked-info` 读产物 exe 的 `.aipyfn` 节，那是链接器合并出来的结果，
也是 VM 运行时看到的同一份数据。

用法：check_pylib.py <pcc.exe> <build 目录> <lib 目录>
"""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))

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

# 只用 upper 一个函数：另外两个函数的 obj 不该被抽出。
ONLY_UPPER = '''import text

print(text.upper("hi"))
'''


def build_exe(pcc, script, output, build_dir, lib_dir):
    command = [pcc, '--exe', script, output, '--build-dir', build_dir]
    if lib_dir:
        command += ['--lib-dir', lib_dir]
    return subprocess.run(command, capture_output=True, text=True,
                          encoding='utf-8')


def linked_info(pcc, exe):
    """读产物里实际链进来的扩展。返回 (模块列表, [(模块, 函数)])。"""
    completed = subprocess.run([pcc, '--linked-info', exe],
                               capture_output=True, text=True, encoding='utf-8')
    if completed.returncode != 0:
        raise SystemExit('读不到 %s 的注册节：%s'
                         % (os.path.basename(exe), completed.stderr.strip()))
    # 输出形如两行 `"modules": [...]` / `"functions": [...]`，包一层花括号
    # 就能被 json 解析。
    payload = json.loads('{' + completed.stdout + '}')
    functions = [(entry[0], entry[1]) for entry in payload['functions']]
    return payload['modules'], functions


def run(exe):
    completed = subprocess.run([exe], capture_output=True, text=True,
                               encoding='utf-8')
    return completed.returncode, completed.stdout, completed.stderr


def main(argv):
    if len(argv) < 4:
        print('用法: check_pylib.py <pcc.exe> <build> <libdir>',
              file=sys.stderr)
        return 2
    pcc = os.path.abspath(argv[1])
    build_dir = os.path.abspath(argv[2])
    lib_dir = os.path.abspath(argv[3])
    for path in (pcc,):
        if not os.path.isfile(path):
            print('找不到 %s' % path, file=sys.stderr)
            return 2

    failures = []
    scratch = tempfile.mkdtemp(prefix='aipy_pylib_')

    # ── 1. 链了 pylib：必须能调到 ──
    script = os.path.join(scratch, 'use_text.py')
    with open(script, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(USE_TEXT)
    output = os.path.join(scratch, 'with_pylib.exe')
    result = build_exe(pcc, script, output, build_dir, lib_dir)
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

        # 三个函数都该在产物里，且模块注册项在。
        modules, functions = linked_info(pcc, output)
        wanted = {('text', 'upper'), ('text', 'repeat'), ('text', 'shout')}
        if set(functions) != wanted or 'text' not in modules:
            failures.append('产物里的注册项不对：模块=%r 函数=%r'
                            % (modules, functions))
            print('  ✗ 产物里的注册项不对')
        else:
            print('  ✓ 产物注册项：%s' % ', '.join(
                '%s.%s' % entry for entry in sorted(functions)))

    # ── 2. 不用扩展：产物里不该有 pylib 的代码 ──
    plain_script = os.path.join(scratch, 'plain.py')
    with open(plain_script, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(PLAIN)
    plain_exe = os.path.join(scratch, 'plain.exe')
    result = build_exe(pcc, plain_script, plain_exe, build_dir, lib_dir)
    if result.returncode != 0:
        failures.append('不用扩展的打包失败：%s' % result.stderr.strip())
        print('  ✗ 不用扩展的打包失败')
    else:
        code, out, err = run(plain_exe)
        modules, functions = linked_info(pcc, plain_exe)
        if code != 0 or out.strip() != '3':
            failures.append('不用扩展的产物跑不对：rc=%d out=%r' % (code, out))
            print('  ✗ 不用扩展的产物跑不对')
        elif modules or functions:
            failures.append('不用扩展的产物里居然有注册项：模块=%r 函数=%r'
                            % (modules, functions))
            print('  ✗ 按需失效：不用扩展却把 pylib 打进去了')
        else:
            print('  ✓ 按需生效：不用扩展的产物里没有任何注册项')

    # ── 3. 函数级按需：只调 upper，另外两个函数的 obj 不该进产物 ──
    upper_script = os.path.join(scratch, 'only_upper.py')
    with open(upper_script, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(ONLY_UPPER)
    upper_exe = os.path.join(scratch, 'only_upper.exe')
    result = build_exe(pcc, upper_script, upper_exe, build_dir, lib_dir)
    if result.returncode != 0:
        failures.append('只调 upper 的打包失败：%s' % result.stderr.strip())
        print('  ✗ 只调 upper 的打包失败')
    else:
        modules, functions = linked_info(pcc, upper_exe)
        if functions != [('text', 'upper')]:
            failures.append('只调 upper 却链进了 %r' % (functions,))
            print('  ✗ 函数级按需失效：%r' % (functions,))
        else:
            code, out, err = run(upper_exe)
            if code != 0 or out.strip() != 'HI':
                failures.append('只调 upper 的运行不对：rc=%d out=%r'
                                % (code, out))
                print('  ✗ 只调 upper 的运行不对')
            else:
                print('  ✓ 函数级按需：产物里只有 text.upper')

    # ── 4. 用了扩展但没链：必须报出可读的错误，不能静默 ──
    info = subprocess.run([pcc, '--info',
                           _bytecode_of(pcc, script, scratch)],
                          capture_output=True, text=True, encoding='utf-8')
    if info.returncode != 0:
        failures.append('读不到字节码结构：%s' % info.stderr.strip())
        print('  ✗ 读不到字节码结构')
    else:
        program = json.loads(info.stdout)
        expected_extensions = ['text.upper', 'text.repeat', 'text.shout']
        if program['extensions'] != expected_extensions:
            failures.append('扩展表内容不对：%r（期望 %r）'
                            % (program['extensions'], expected_extensions))
            print('  ✗ 扩展表内容不对')
        else:
            print('  ✓ 扩展表：%s' % ', '.join(program['extensions']))

        # 造一个「字节码要 text.upper，但产物没链任何 pylib」的样本。
        # `--bundle` 只做最小链接（只有语言运行时 + launcher），不点名任何
        # 扩展符号，于是运行时会报「text.upper 没有链接进来」。
        #
        # 这会真的调一次 link.exe，所以要把构建配方与库目录一起给它。
        broken = os.path.join(scratch, 'no_pylib.exe')
        bundle = subprocess.run(
            [pcc, '--bundle', _bytecode_of(pcc, script, scratch), broken,
             '--build-dir', build_dir, '--lib-dir', lib_dir],
            capture_output=True, text=True, encoding='utf-8')
        if bundle.returncode != 0:
            failures.append('拼装裸产物失败：%s'
                            % (bundle.stderr.strip() or bundle.stdout.strip()))
            print('  ✗ 拼装裸产物失败')
        else:
            code, out, err = run(broken)
            if code == 0:
                failures.append('没链 pylib 却跑成功了（应当报错）')
                print('  ✗ 没链 pylib 却静默成功')
            elif 'text.upper' not in err:
                failures.append('报错没提到具体函数：%r' % err.strip())
                print('  ✗ 报错没提到具体函数')
            else:
                print('  ✓ 没链 pylib：%s' % err.strip())

    check_subsystem(pcc, build_dir, lib_dir, scratch, failures)

    if failures:
        print('\n失败：', file=sys.stderr)
        for line in failures:
            print('  ' + line, file=sys.stderr)
        return 1
    print('\npylib 按需链接全部通过。')
    return 0


def _bytecode_of(pcc, script, scratch):
    """把脚本编成 .bc，返回路径。"""
    output = os.path.join(scratch, 'probe.bc')
    completed = subprocess.run([pcc, script, output], capture_output=True,
                               text=True, encoding='utf-8')
    if completed.returncode != 0:
        raise SystemExit('编译 %s 失败：%s'
                         % (os.path.basename(script), completed.stderr.strip()))
    return output


# ── 子系统：控制台程序 vs 图形界面程序 ──
#
# 判据必须读 PE 头，不能信打包器的自述。`/subsystem` 写错位置会被链接器
# 忽略而不报错，产物照样能跑——只是双击时行为不对。
#
# 有 `class X(Activity)` 就是 GUI，否则是控制台。`Activity` 是语言里唯一的
# 类形态，也是「这个脚本有界面」的确切信号。
GUI_SCRIPT = '''class App(Activity):
    def on_start(self):
        pass


print("gui ran")
'''


def _dumpbin(pcc):
    """从 pcc 找到的链接器路径推出 dumpbin（同一目录）。"""
    import shutil as _shutil
    for directory in os.environ.get('PATH', '').split(os.pathsep):
        candidate = os.path.join(directory, 'dumpbin.exe')
        if os.path.isfile(candidate):
            return candidate
    found = _shutil.which('dumpbin')
    return found


def subsystem_of(dumpbin, exe):
    """读 PE 头里的子系统。返回 'console' / 'gui' / None。"""
    if not dumpbin:
        return None
    completed = subprocess.run([dumpbin, '/headers', exe], capture_output=True,
                               text=True, encoding='utf-8', errors='replace')
    for line in (completed.stdout or '').splitlines():
        if 'subsystem (' not in line:
            continue
        if 'Windows GUI' in line:
            return 'gui'
        if 'Windows CUI' in line:
            return 'console'
    return None


def check_subsystem(pcc, build_dir, lib_dir, scratch, failures):
    """纯脚本 → 控制台；有 Activity → GUI。"""
    print()
    print('=== 产物子系统 ===')
    dumpbin = _dumpbin(pcc)
    if dumpbin is None:
        # 找不到 dumpbin 不算失败：它是可选的诊断工具，用户机器上只有
        # link.exe 也合法。但要说清「这条没验」，不能静默跳过。
        print('  - 跳过（找不到 dumpbin.exe）')
        return

    cases = [('console_plain', PLAIN, 'console'),
             ('gui_activity', GUI_SCRIPT, 'gui')]
    for name, source, expected in cases:
        script = os.path.join(scratch, name + '.py')
        with open(script, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(source)
        exe = os.path.join(scratch, name + '.exe')
        result = build_exe(pcc, script, exe, build_dir, lib_dir)
        if result.returncode != 0:
            failures.append('%s 打包失败：%s'
                            % (name, result.stderr.strip() or result.stdout.strip()))
            print('  ✗ %s 打包失败' % name)
            continue
        actual = subsystem_of(dumpbin, exe)
        if actual != expected:
            failures.append('%s 的子系统是 %r，期望 %r' % (name, actual, expected))
            print('  ✗ %-14s 子系统 %r，期望 %r' % (name, actual, expected))
        else:
            print('  ✓ %-14s 子系统 %s' % (name, expected))


if __name__ == '__main__':
    sys.exit(main(sys.argv))
