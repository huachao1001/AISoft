#!/usr/bin/env python3
"""`fs` / `time` / `json` 三个系统库的端到端验证。

── 验什么 ──

三个库共用一套结构，所以一次验完。每个库都要满足两条：

1. **功能**：脚本调用能拿到与 CPython 同义的结果。
2. **函数级按需**：只调一个函数时，库里其余函数的 obj 不进产物。

第 2 条是这个项目的核心承诺（Design.md 第 1.3 条），而它只能靠「真的
链接一次、再看链接器收到哪些符号」来验——源码层面看不出来。

── 为什么错误路径也要验 ──

三个库都没有异常通道，失败靠返回值表示（None / False / -1）。这类
「约定」最容易在改动中退化：某个函数改成返回空列表而不是 None，调用方
的 `if x == None` 会静默失效。

用法：check_system_libs.py <aipyic.py> <build 目录> <lib 目录> <python>
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))

FS_SCRIPT = '''import fs

DIR = "work"
print("mkdir " + str(fs.mkdir(DIR)))

path = DIR + "/a.txt"
print("write " + str(fs.write_text(path, "line1\\nline2\\n")))
print("exists " + str(fs.exists(path)))
print("size_ok " + str(fs.size(path) > 0))
print("is_file " + str(fs.is_file(path)))
print("is_dir " + str(fs.is_dir(DIR)))

text = fs.read_text(path)
print("read " + str(len(text)))

fs.append_text(path, "line3\\n")
print("append " + str(fs.size(path) > 0))

entries = fs.listdir(DIR)
entries.sort()
print("list " + str(entries))

# 不存在的文件：read_text 返回 None，不是空串。
print("missing " + str(fs.read_text(DIR + "/nope.txt") == None))
print("missing_size " + str(fs.size(DIR + "/nope.txt")))

# with 文件对象：退出必关。
with fs.open(path, "w") as handle:
    handle.write("rewritten")
print("with_write " + str(fs.read_text(path)))

print("remove " + str(fs.remove(path)))
print("remove_missing " + str(fs.remove(path)))
'''

FS_EXPECTED = '''mkdir True
write True
exists True
size_ok True
is_file True
is_dir True
read 12
append True
list ['a.txt']
missing True
missing_size -1
with_write rewritten
remove True
remove_missing False
'''

TIME_SCRIPT = '''import time

now = time.time()
print("time_ok " + str(now > 1600000000.0))

a = time.monotonic()
time.sleep(0.02)
b = time.monotonic()
print("sleep_ok " + str(b > a))

parts = time.localtime()
print("localtime_len " + str(len(parts)))
print("year_ok " + str(parts[0] > 2000))

stamp = time.strftime("%Y-%m-%d")
print("strftime_ok " + str(len(stamp) == 10))

# 负数/零 sleep 当 0 处理，不崩。
time.sleep(0)
time.sleep(-1)
print("neg_sleep_ok True")
'''

JSON_SCRIPT = '''import json

data = {"name": "x", "nums": [1, 2, 3], "flag": True}
encoded = json.dumps(data)
print(encoded)

back = json.loads(encoded)
print("name " + back["name"])
print("nums " + str(back["nums"]))
print("flag " + str(back["flag"]))

# 整数与浮点要区分开（与 CPython 的 json 一致）。
print("int " + str(json.loads("1") == 1))
print("float_ok " + str(json.loads("1.5") == 1.5))

# 转义往返。
print("escape " + json.loads(json.dumps("a\\"b\\nc"))) 

# 嵌套。
nested = json.loads('{"a": {"b": [1, {"c": 2}]}}')
print("nested " + str(nested["a"]["b"][1]["c"]))

# 失败返回 None。
print("bad_json " + str(json.loads("{oops") == None))
print("non_string_key " + str(json.dumps({1: 2}) == None))
'''

NONE_OK = 'print("none_ok " + str(None == None))\n'


def build_exe(aipyic, script, output, build_dir, lib_dir, python):
    command = [python, aipyic, '--exe', script, output, '--build-dir', build_dir]
    if lib_dir:
        command += ['--lib-dir', lib_dir]
    return subprocess.run(command, capture_output=True, text=True, encoding='utf-8')


def run(exe, cwd=None, timeout=30):
    try:
        completed = subprocess.run([exe], capture_output=True, text=True,
                                   encoding='utf-8', timeout=timeout, cwd=cwd)
        return completed.returncode, completed.stdout, completed.stderr
    except subprocess.TimeoutExpired:
        return -1, '', '超时'


def main(argv):
    if len(argv) < 5:
        print('用法: check_system_libs.py <aipyic.py> <build> <libdir> <python>',
              file=sys.stderr)
        return 2
    aipyic = os.path.abspath(argv[1])
    build_dir = os.path.abspath(argv[2])
    lib_dir = os.path.abspath(argv[3])
    python = argv[4]

    sys.path.insert(0, os.path.join(AIPY, 'tools', 'aipyic'))
    import compiler
    import link

    failures = []
    scratch = tempfile.mkdtemp(prefix='aipy_system_')

    def write(name, text):
        path = os.path.join(scratch, name)
        with open(path, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(text)
        return path

    def case(label, text, name, expected, cwd=None):
        script = write(name + '.py', text)
        exe = os.path.join(scratch, name + '.exe')
        result = build_exe(aipyic, script, exe, build_dir, lib_dir, python)
        if result.returncode != 0:
            failures.append('%s 打包失败：%s'
                            % (label, result.stderr.strip() or result.stdout.strip()))
            print('  ✗ %s 打包失败' % label)
            return False
        code, out, err = run(exe, cwd=cwd)
        if code != 0 or out != expected:
            failures.append('%s 结果不符：rc=%d\n  期望 %r\n  实际 %r\n  错误 %r'
                            % (label, code, expected, out, err.strip()))
            print('  ✗ %s 结果不符' % label)
            return False
        print('  ✓ %s（%d 行输出）' % (label, len(expected.splitlines())))
        return True

    TIME_EXPECTED = ('time_ok True\nsleep_ok True\nlocaltime_len 7\nyear_ok True\n'
                     'strftime_ok True\nneg_sleep_ok True\n')
    JSON_EXPECTED = ('{"name":"x","nums":[1,2,3],"flag":true}\n'
                     'name x\nnums [1, 2, 3]\nflag True\n'
                     'int True\nfloat_ok True\n'
                     'escape a"b\nc\n'
                     'nested 2\nbad_json True\nnon_string_key True\n')

    # fs 会在 cwd 下建目录，所以给它一个干净的工作目录。
    fs_work = os.path.join(scratch, 'fs_work')
    os.makedirs(fs_work, exist_ok=True)
    case('fs 功能', FS_SCRIPT, 'fs_case', FS_EXPECTED, cwd=fs_work)
    case('time 功能', TIME_SCRIPT, 'time_case', TIME_EXPECTED)
    case('json 功能', JSON_SCRIPT, 'json_case', JSON_EXPECTED)

    # ── 函数级按需：每个库只点一个函数 ──    print()
    checks = [
        ('fs', 'import fs\nprint(fs.exists("."))\n', 'exists'),
        ('time', 'import time\nprint(time.time() > 0)\n', 'time'),
        ('json', 'import json\nprint(json.dumps(1))\n', 'dumps'),
    ]
    for module, source, wanted in checks:
        script = write('only_%s.py' % module, source)
        program = compiler.compile_file(script)
        probe = os.path.join(scratch, 'probe_%s.exe' % module)
        try:
            result = link.relink(build_dir, lib_dir, [module], probe,
                                 extensions=program['extensions'])
        except link.LinkError as error:
            failures.append('%s 探针链接失败：%s' % (module, error))
            print('  ✗ %s 探针链接失败' % module)
            continue

        symbols = result.symbols
        # 必须点名：目标函数 + 模块注册项。不能有别的函数的符号。
        expected_min = {'aipy_%s_fn_%s' % (module, wanted),
                        'aipy_pylib_entry_%s' % module}
        extra = [s for s in symbols if s not in expected_min]
        if extra:
            failures.append('%s 只调 %s，却多点了 %s'
                            % (module, wanted, ', '.join(extra)))
            print('  ✗ %s 函数级按需失效：多点了 %s' % (module, ', '.join(extra)))
        elif not expected_min.issubset(set(symbols)):
            failures.append('%s 缺少必要符号：期望 %s，实际 %s'
                            % (module, sorted(expected_min), symbols))
            print('  ✗ %s 缺少必要符号' % module)
        else:
            print('  ✓ %s 函数级按需：只点名 %s' % (module, ', '.join(sorted(expected_min))))

    print()
    if failures:
        print('失败：', file=sys.stderr)
        for line in failures:
            print('  ' + line, file=sys.stderr)
        return 1
    print('fs / time / json 三个系统库全部通过。')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
