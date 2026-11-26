#!/usr/bin/env python3
"""`fs.open` 与 CPython `open` 的逐行对拍。

── 为什么用对拍而不是写死期望值 ──

写死的期望值反映的是「作者以为 Python 怎么做」。文件对象最容易在这几处
记错：`write` 返回字符数（不是字节数）、`readline` 保留行尾 `\\n`、
`writelines` 不自动加换行、`seek` 允许越过末尾。这些细节人写十次错三次，
而 CPython 就在手边——让它说。

── 怎么对拍 ──

同一段逻辑写两遍：一份用 CPython 的 `open`，一份用 `fs.open`，差别只有
三处，都用占位符标出来：

    OPEN(...)       -> open(...)          / fs.open(...)
    CLOSED(f)       -> f.closed           / f.closed()
    show(text)      -> 两边相同（自己转义，不依赖 repr）

文本模式下 Windows 的 CPython 会把 `\\n` 写成 `\\r\\n`，AiPy 也照做，
所以两边的输出与磁盘字节都应当一致。

用法：check_fs_open.py <pcc.exe> <build> <libdir>
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))

# `show` 自己转义，不依赖 repr：AiPy 没有 repr 内建，而 `f.closed` 是属性，
# 这里必须写成方法。这两处正是语言子集的已知边界，写进占位符反而让两份
# 脚本的差异一目了然。
PROBE = '''
import fs

DIR = "probe_work"


def show(text):
    return text.replace("\\\\", "\\\\\\\\").replace("\\r", "\\\\r").replace("\\n", "\\\\n")


def header(text):
    print("== " + text)


# ── 1. 写、读回、write 的返回值是字符数 ──
path = DIR + "/basic.txt"
with OPEN(path, "w") as f:
    print("write_ret " + str(f.write("第一行\\n")))
    print("write_ascii " + str(f.write("abc")))
header("basic")
with OPEN(path) as f:
    total = f.read()
    print("read_len " + str(len(total)))
    print("at_eof " + str(len(f.read())))

# ── 2. readline 保留行尾 ──
path2 = DIR + "/lines.txt"
path6b = DIR + "/seekbin.txt"
with OPEN(path2, "w") as f:
    f.write("a\\nb\\nc\\n")
header("readline")
with OPEN(path2) as f:
    print("line1 " + show(f.readline()))
    print("line2 " + show(f.readline()))
    print("rest " + show(f.read()))

# ── 3. read(n) 在文本模式按字符，不按字节 ──
path3 = DIR + "/cn.txt"
with OPEN(path3, "w") as f:
    f.write("中文测试")
header("read_n")
with OPEN(path3) as f:
    print("first2 " + f.read(2))
    print("next2 " + f.read(2))

# ── 4. readlines 与 for 迭代 ──
header("readlines")
with OPEN(path2) as f:
    lines = f.readlines()
    print("count " + str(len(lines)))
    print("first " + show(lines[0]))
    print("last " + show(lines[2]))
with OPEN(path2) as f:
    collected = []
    for line in f:
        collected.append(line.strip())
    print("iter " + str(collected))

# ── 5. writelines 不自动加换行 ──
path5 = DIR + "/wl.txt"
with OPEN(path5, "w") as f:
    f.writelines(["x", "y", "z"])
header("writelines")
with OPEN(path5) as f:
    print("raw " + show(f.read()))

path6b = DIR + "/seekbin.txt"

# ── 6. seek / tell（对拍用字节偏移：文本模式的 tell 值两边无法一致）──
header("seek")
with OPEN(path6b, "w") as f:
    f.write("abc")
with OPEN(path6b, "rb") as f:
    print("tell0 " + str(f.tell()))
    f.seek(2)
    print("tell2 " + str(f.tell()))
    f.seek(0, 2)
    print("end " + str(f.tell()))

# ── 7. 追加模式 ──
path7 = DIR + "/ap.txt"
with OPEN(path7, "w") as f:
    f.write("start")
with OPEN(path7, "a") as f:
    f.write("+more")
header("append")
with OPEN(path7) as f:
    print("content " + show(f.read()))

# ── 8. with 自动关，显式 close 也生效 ──
header("close")
f = OPEN(path2)
print("closed_before " + str(CLOSED(f)))
f.close()
print("closed_after " + str(CLOSED(f)))

# ── 9. r+ 覆盖写：只替换写到的位置，不截断整个文件 ──
path9 = DIR + "/over.txt"
with OPEN(path9, "w") as f:
    f.write("0123456789")
with OPEN(path9, "r+") as f:
    f.write("ab")
header("overwrite")
with OPEN(path9) as f:
    print("after_overwrite " + show(f.read()))

# ── 10. 读写能力查询 ──
header("capability")
with OPEN(path2, "r") as f:
    print("r_readable " + str(f.readable()))
    print("r_writable " + str(f.writable()))
    print("r_seekable " + str(f.seekable()))
with OPEN(path2, "w") as f:
    print("w_readable " + str(f.readable()))
    print("w_writable " + str(f.writable()))

# ── 11. x 模式：已存在必须失败 ──
# 两边都抛 FileExistsError，所以这里直接对拍，不需要占位符。
header("exclusive")
try:
    OPEN(path2, "x")
    print("x_exists False")
except FileExistsError:
    print("x_exists True")
path11 = DIR + "/new.txt"
with OPEN(path11, "x") as f:
    f.write("fresh")
with OPEN(path11) as f:
    print("x_content " + show(f.read()))

# ── 12. flush 后内容已经落盘 ──
header("flush")
path12 = DIR + "/fl.txt"
f = OPEN(path12, "w")
f.write("flushed")
f.flush()
with OPEN(path12) as other:
    print("visible " + show(other.read()))
f.close()

# ── 13. seek 越过末尾再写：补零 ──
header("sparse")
path13 = DIR + "/sparse.txt"
with OPEN(path13, "w") as f:
    f.write("ab")
    f.seek(5)
    f.write("z")
with OPEN(path13, "rb") as f:
    print("sparse_len " + str(len(f.read())))
'''


def build_exe(pcc, script, output, build_dir, lib_dir):
    command = [pcc, '--exe', script, output, '--build-dir', build_dir]
    if lib_dir:
        command += ['--lib-dir', lib_dir]
    return subprocess.run(command, capture_output=True, text=True, encoding='utf-8')


def main(argv):
    if len(argv) < 4:
        print('用法: check_fs_open.py <pcc.exe> <build> <libdir>',
              file=sys.stderr)
        return 2
    pcc = os.path.abspath(argv[1])
    build_dir = os.path.abspath(argv[2])
    lib_dir = os.path.abspath(argv[3])
    if not os.path.isfile(pcc):
        print('找不到 %s' % pcc, file=sys.stderr)
        return 2

    scratch = tempfile.mkdtemp(prefix='aipy_fsopen_')
    failures = []

    cpython_source = (PROBE.replace('OPEN(', 'open(')
                           .replace('CLOSED(f)', 'f.closed')
                           .replace('import fs\n', ''))
    aipy_source = (PROBE.replace('OPEN(', 'fs.open(')
                        .replace('CLOSED(f)', 'f.closed()'))

    def write(name, text):
        path = os.path.join(scratch, name)
        with open(path, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(text)
        return path

    cpython_script = write('cpython_probe.py', cpython_source)
    aipy_script = write('aipy_probe.py', aipy_source)

    cpython_work = os.path.join(scratch, 'cpython_work')
    aipy_work = os.path.join(scratch, 'aipy_work')

    # 目录由驱动建好：探针要同时能在 CPython 与 AiPy 里跑，目标路径也不该
    # 混进脚本主体——放进去就破坏「只差三处」的前提。
    os.makedirs(os.path.join(cpython_work, 'probe_work'), exist_ok=True)
    os.makedirs(os.path.join(aipy_work, 'probe_work'), exist_ok=True)

    # ── 文本模式的 seek/tell：与 CPython 刻意不同，必须显式断言 ──
    #
    # CPython 文本模式的 tell() 返回「把换行转换算进去的不透明 cookie」：
    # 写 `a\n` 之后 tell 是 3（Windows）而内容是 2 个字符；它的 seek 也只
    # 接受这种 cookie。这里的位置是解码后文本的字符下标——可预测、可算，
    # 但与 CPython 的数值不对应。
    #
    # 这个差异不能默默存在：脚本如果在 CPython 里靠 tell() 记住位置，搬到
    # 这里会拿到不同数字。所以要有一条断言把它钉住，改动时立刻看见。
    text_seek_script = write('text_seek.py', '''
import fs

with fs.open("probe_work/ts.txt", "w") as f:
    f.write("ab\\ncd\\n")

# 文本模式：position 是字符下标，tell 直接返回它。
with fs.open("probe_work/ts.txt") as f:
    print("tell " + str(f.tell()))
    print("read2 " + f.read(2))
    print("tell_after " + str(f.tell()))
    f.seek(3)
    print("read_from3 " + f.read(1))
''')
    text_seek_exe = os.path.join(scratch, 'text_seek.exe')
    built = build_exe(pcc, text_seek_script, text_seek_exe, build_dir,
                      lib_dir)
    if built.returncode != 0:
        print('文本模式 seek 探针打包失败：', file=sys.stderr)
        print(built.stderr or built.stdout, file=sys.stderr)
        return 1
    seek_result = subprocess.run([text_seek_exe], capture_output=True, text=True,
                                 encoding='utf-8', cwd=aipy_work)
    expected_text_seek = 'tell 0\nread2 ab\ntell_after 2\nread_from3 c\n'
    if seek_result.returncode != 0 or seek_result.stdout != expected_text_seek:
        failures.append(('文本模式 seek/tell 的约定',
                         expected_text_seek.splitlines(),
                         seek_result.stdout.splitlines()))

    # 目录由驱动建好：探针要同时能在 CPython 与 AiPy 里跑，目标路径也不该
    # 混进脚本主体——放进去就破坏「只差三处」的前提。
    os.makedirs(os.path.join(cpython_work, 'probe_work'), exist_ok=True)

    baseline = subprocess.run([sys.executable, cpython_script], capture_output=True,
                              text=True, encoding='utf-8', cwd=cpython_work)
    if baseline.returncode != 0:
        print('CPython 基准自己就失败了，无法对拍：', file=sys.stderr)
        print(baseline.stderr, file=sys.stderr)
        return 2

    exe = os.path.join(scratch, 'aipy_probe.exe')
    built = build_exe(pcc, aipy_script, exe, build_dir, lib_dir)
    if built.returncode != 0:
        print('打包失败：', file=sys.stderr)
        print(built.stderr or built.stdout, file=sys.stderr)
        return 1

    result = subprocess.run([exe], capture_output=True, text=True,
                            encoding='utf-8', cwd=aipy_work)

    # 按 header 分组比对：某组不一致时只说那一组，不刷整屏。
    def group_lines(text):
        groups = {}
        current = '(开头)'
        groups.setdefault(current, [])
        for line in text.splitlines():
            if line.startswith('== '):
                current = line[3:]
                groups.setdefault(current, [])
            else:
                groups[current].append(line)
        return groups

    left_groups = group_lines(baseline.stdout)
    right_groups = group_lines(result.stdout)

    for name in left_groups:
        left = left_groups[name]
        right = right_groups.get(name)
        if right is None:
            failures.append((name, left, ['(整组缺失)']))
            continue
        if left != right:
            failures.append((name, left, right))

    if result.returncode != 0:
        failures.append(('退出码', ['0'], [str(result.returncode)]))

    if failures:
        print('fs.open 与 CPython 不一致：', file=sys.stderr)
        for name, left, right in failures:
            print('  ── %s ──' % name, file=sys.stderr)
            for i in range(max(len(left), len(right))):
                want = left[i] if i < len(left) else '(缺)'
                got = right[i] if i < len(right) else '(缺)'
                flag = ' ' if want == got else '≠'
                print('   %s cpython: %-40s aipy: %s' % (flag, want, got),
                      file=sys.stderr)
        if result.stderr.strip():
            print('  诊断输出：', file=sys.stderr)
            for line in result.stderr.strip().splitlines():
                print('    ' + line, file=sys.stderr)
        return 1

    # 磁盘字节也必须一致：文本模式下两边都该把 \n 写成 \r\n。
    # 要递归进 probe_work 子目录——所有探针文件都在里面，只看顶层会得到
    # 「一个目录、零个文件」这种假通过。
    byte_failures = []
    compared = 0
    for root, _dirs, files in os.walk(cpython_work):
        for name in sorted(files):
            left = os.path.join(root, name)
            relative = os.path.relpath(left, cpython_work)
            right = os.path.join(aipy_work, relative)
            compared += 1
            if not os.path.isfile(right):
                byte_failures.append((relative, '存在', '不存在'))
                continue
            with open(left, 'rb') as handle:
                left_bytes = handle.read()
            with open(right, 'rb') as handle:
                right_bytes = handle.read()
            if left_bytes != right_bytes:
                byte_failures.append((relative, repr(left_bytes),
                                      repr(right_bytes)))

    if byte_failures:
        print('磁盘内容不一致：', file=sys.stderr)
        for name, left, right in byte_failures:
            print('  %s: cpython=%s aipy=%s' % (name, left, right), file=sys.stderr)
        return 1

    print('fs.open 与 CPython 一致：%d 组、%d 行输出、%d 个文件字节相同'
          % (len(left_groups), len(result.stdout.splitlines()), compared))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
