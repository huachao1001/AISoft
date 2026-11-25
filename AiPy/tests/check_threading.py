#!/usr/bin/env python3
"""`threading` pylib 的端到端验证。

── 验什么 ──

线程是第一个「具体能力库」，它同时验证三件独立的事：

1. **功能**：脚本函数真的在后台线程执行，结果回到主线程。
2. **函数级按需**：只调 `threading.pending` 的产物里不该有 `Thread`/`start`
   的代码。这条是设计理念第二条的落点，也是与模块级链接的关键区别。
3. **标准库对齐**：`Thread` / `Lock` / `Event` 的方法名与语义和 CPython
   `threading` 一致（差别只有 target 写名字）。

第 2 条只能真的链接两次、再看链接器收到的符号来验——`/INCLUDE` 没点到名
的 obj 根本不会被抽出，而它「没被抽出」在源码层面看不出来。

用法：check_threading.py <aipyic.py> <build 目录> <lib 目录> <python>
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))

# 两个函数分别在不同线程跑，用 waitAll 收结果。
# 结果顺序与完成顺序有关，所以脚本内排序后再打印，避免 flaky。
USE_THREAD = '''import threading


def square(n):
    return n * n


def greet(name):
    return "hello " + name


threading.start("square", 7)
threading.start("greet", "world")

values = threading.waitAll()
values.sort()
for item in values:
    print(item)
'''

EXPECTED = "49\nhello world\n"

# 只调 pending：不该需要 start/Thread/waitAll 的任何代码。
ONLY_PENDING = '''import threading

print(threading.pending())
'''

# 用到不存在的函数：必须在提交时就失败，返回 0 而不是静默排队。
BAD_FUNCTION = '''import threading

print(threading.start("no_such_function", 1))
'''

# with 的完整语义：return / break / continue 都必须调 __exit__。
# 漏一条的症状是「第二次进锁永久阻塞」，所以最后再进一次是核心断言。
WITH_EXIT = '''import threading

lock = threading.Lock()


def early_return():
    with lock:
        return "returned"


def early_break():
    for i in range(3):
        with lock:
            if i == 1:
                break
    return "broke"


print(early_return())
print(early_break())

with lock:
    print("still alive")
'''

EXPECTED_WITH = "returned\nbroke\nstill alive\n"

# current_thread：主线程叫 MainThread，worker 叫 Worker-N。
CURRENT_THREAD = '''import threading


def whoami():
    return threading.current_thread()


print(threading.current_thread())
threading.start("whoami")
names = threading.waitAll()
print(names[0])
'''

# Thread 对象：start/join/is_alive/name 与标准库同名同义。
THREAD_OBJECT = '''import threading

TOTAL = 0
lock = threading.Lock()


def bump(n):
    global TOTAL
    with lock:
        TOTAL = TOTAL + n


t = threading.Thread("bump", 5)
print(t.name())
print(t.is_alive())
t.start()
t.join()
print(t.is_alive())
print(TOTAL)
'''

EXPECTED_THREAD_OBJECT = "bump\nFalse\nFalse\n5\n"

# 两次 start 同一个 Thread 是无效的（CPython 会抛 RuntimeError，这里返回
# False 并保持线程只跑一次）。
THREAD_DOUBLE_START = '''import threading

COUNT = 0
lock = threading.Lock()


def once():
    global COUNT
    with lock:
        COUNT = COUNT + 1


t = threading.Thread("once")
t.start()
t.start()
t.join()
print(COUNT)
'''

# Event：set/is_set/wait 与标准库同义。
EVENT = '''import threading

ready = threading.Event()
print(ready.is_set())
ready.set()
print(ready.is_set())
ready.wait()
print("waited")
ready.clear()
print(ready.is_set())
'''

EXPECTED_EVENT = "False\nTrue\nwaited\nFalse\n"

# 提交时参数个数校验：Thread("bump") 少一个参数，start 返回 False，
# join 立刻返回，不阻塞、不崩溃。
THREAD_BAD_ARGS = '''import threading


def bump(n):
    return n


t = threading.Thread("bump")
t.start()
t.join()
print("done")
'''

EXPECTED_THREAD_BAD_ARGS = "done\n"


def build_exe(aipyic, script, output, build_dir, lib_dir, python):
    command = [python, aipyic, '--exe', script, output, '--build-dir', build_dir]
    if lib_dir:
        command += ['--lib-dir', lib_dir]
    return subprocess.run(command, capture_output=True, text=True, encoding='utf-8')


def run(exe, timeout=30):
    try:
        completed = subprocess.run([exe], capture_output=True, text=True,
                                   encoding='utf-8', timeout=timeout)
        return completed.returncode, completed.stdout, completed.stderr
    except subprocess.TimeoutExpired:
        return -1, '', '超时（可能的死锁）'


def main(argv):
    if len(argv) < 5:
        print('用法: check_threading.py <aipyic.py> <build> <libdir> <python>',
              file=sys.stderr)
        return 2
    aipyic = os.path.abspath(argv[1])
    build_dir = os.path.abspath(argv[2])
    lib_dir = os.path.abspath(argv[3])
    python = argv[4]

    # 直接拿 link 模块来查真实的 /INCLUDE 符号清单。
    #
    # 为什么不解析打包摘要：摘要只是打包器「自己说要链什么」，而函数级
    # 按需要验的是「链接器实际收到了什么」。两者可能不一致——最初这个
    # 测试就是因为信了摘要，在故意注入「全点名」后仍然报通过。
    sys.path.insert(0, os.path.join(AIPY, 'tools', 'aipyic'))
    import link

    failures = []
    scratch = tempfile.mkdtemp(prefix='aipy_threading_')

    def write(name, text):
        path = os.path.join(scratch, name)
        with open(path, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(text)
        return path

    def build_and_run(label, text, name, expected, timeout=30):
        """打包并跑一份脚本，比对完整输出。返回是否通过。"""
        script = write(name + '.py', text)
        exe = os.path.join(scratch, name + '.exe')
        result = build_exe(aipyic, script, exe, build_dir, lib_dir, python)
        if result.returncode != 0:
            failures.append('%s 打包失败：%s'
                            % (label, result.stderr.strip() or result.stdout.strip()))
            print('  ✗ %s 打包失败' % label)
            return False
        code, out, err = run(exe, timeout=timeout)
        if code != 0 or out != expected:
            failures.append('%s 结果不符：rc=%d out=%r err=%r'
                            % (label, code, out, err.strip()))
            print('  ✗ %s 结果不符' % label)
            return False
        print('  ✓ %s：%s' % (label, out.replace('\n', ' | ').strip()))
        return True

    # ── 1. 功能：脚本函数真的在后台跑，结果正确 ──
    if build_and_run('后台线程结果', USE_THREAD, 'use_thread', EXPECTED):
        # 打包摘要里应当只列真的调用到的函数。
        script = os.path.join(scratch, 'use_thread.py')
        result = build_exe(aipyic, script, os.path.join(scratch, 'summary.exe'),
                           build_dir, lib_dir, python)
        summary = result.stdout
        if 'threading.start' not in summary or 'threading.waitAll' not in summary:
            failures.append('打包摘要没有列出点名的函数：%r' % summary.strip())
            print('  ✗ 打包摘要缺函数清单')

    # ── 2. 函数级按需：只用 pending，链接器不该收到别的函数 ──
    simple = write('only_pending.py', ONLY_PENDING)
    simple_exe = os.path.join(scratch, 'only_pending.exe')
    result = build_exe(aipyic, simple, simple_exe, build_dir, lib_dir, python)
    if result.returncode != 0:
        failures.append('只调 pending 的打包失败：%s' % result.stderr.strip())
        print('  ✗ 只调 pending 的打包失败')
    else:
        # 查真实符号：这是链接器实际收到的 /INCLUDE 列表。
        import compiler
        program = compiler.compile_file(simple)
        probe_out = os.path.join(scratch, 'probe.exe')
        symbols = []
        try:
            link_result = link.relink(build_dir, lib_dir,
                                      ['threading'], probe_out,
                                      extensions=program.get('extensions'))
            symbols = link_result.symbols
        except link.LinkError as error:
            failures.append('探针链接失败：%s' % error)
            print('  ✗ 探针链接失败')

        expected_symbols = ['aipy_threading_fn_pending', 'aipy_pylib_entry_threading']
        others = ('aipy_threading_fn_start', 'aipy_threading_fn_Thread',
                  'aipy_threading_fn_waitAll', 'aipy_threading_fn_poll',
                  'aipy_threading_fn_Lock', 'aipy_threading_fn_Event')
        unexpected = [name for name in symbols if name in others]
        if unexpected:
            failures.append('只调 pending，链接器却收到 %s' % ', '.join(unexpected))
            print('  ✗ 函数级按需失效：多点了 %s' % ', '.join(unexpected))
        elif sorted(symbols) != sorted(expected_symbols):
            failures.append('点名的符号不对：%s（期望 %s）'
                            % (symbols, expected_symbols))
            print('  ✗ 点名的符号不对：%s' % symbols)
        else:
            code, out, err = run(simple_exe)
            if code != 0 or out.strip() != '0':
                failures.append('只调 pending 的运行不对：rc=%d out=%r' % (code, out))
                print('  ✗ 只调 pending 的运行不对')
            else:
                print('  ✓ 函数级按需：链接器只收到 %s' % ', '.join(symbols))

    # ── 3. 提交时校验：函数不存在必须立刻返回 0，不能静默排队 ──
    build_and_run('函数不存在返回 0', BAD_FUNCTION, 'bad_function', '0\n')

    # ── 4. with 的完整语义：return/break/continue 都调 __exit__ ──
    #
    # 漏一条的症状不是报错，是「第二次进锁永久阻塞」——所以最后再 with 一次
    # 才是真正的断言。这条只能真的跑 exe，没法静态检查。
    build_and_run('with 完整语义', WITH_EXIT, 'with_exit', EXPECTED_WITH, timeout=20)

    # ── 5. current_thread：主线程与 worker 的名字 ──
    name_script = write('current_thread.py', CURRENT_THREAD)
    name_exe = os.path.join(scratch, 'current_thread.exe')
    result = build_exe(aipyic, name_script, name_exe, build_dir, lib_dir, python)
    if result.returncode != 0:
        failures.append('current_thread 打包失败：%s' % result.stderr.strip())
        print('  ✗ current_thread 打包失败')
    else:
        code, out, err = run(name_exe)
        lines = out.split()
        if code != 0 or len(lines) != 2 or lines[0] != 'MainThread' \
                or not lines[1].startswith('Worker-'):
            failures.append('current_thread 名字不对：rc=%d out=%r' % (code, out))
            print('  ✗ current_thread 名字不对')
        else:
            print('  ✓ current_thread：%s / %s' % (lines[0], lines[1]))

    # ── 6. Thread 对象：start/join/is_alive/name ──
    build_and_run('Thread 对象', THREAD_OBJECT, 'thread_object',
                  EXPECTED_THREAD_OBJECT)

    # ── 7. 重复 start 无效（只跑一次） ──
    build_and_run('重复 start 只跑一次', THREAD_DOUBLE_START, 'thread_double', '1\n')

    # ── 8. Event：set/is_set/wait/clear ──
    build_and_run('Event 语义', EVENT, 'event', EXPECTED_EVENT)

    # ── 9. 参数个数不对：start 返回 False、join 不阻塞、不崩溃 ──
    build_and_run('参数个数校验', THREAD_BAD_ARGS, 'thread_bad_args',
                  EXPECTED_THREAD_BAD_ARGS)

    if failures:
        print('\n失败：', file=sys.stderr)
        for line in failures:
            print('  ' + line, file=sys.stderr)
        return 1
    print('\nthreading 库全部通过。')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
