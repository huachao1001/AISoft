#!/usr/bin/env python3
"""示例的可执行验证：每个示例跑两遍，比对输出。

── 为什么示例必须有测试 ──

示例是使用者看到的第一份代码。它一旦过时（内建改名、语法规则收紧），
受害者是「照着示例写」的新用户，而他们通常不会去读源码确认哪边错了。
手工跑一遍再提交靠不住——改 40 行脚本很容易漏掉最后一个示例。

── 怎么判「一致」──

1. `01_basics.py`、`02_wordcount.py`、`04_tool.py` 用**纯 Python 子集**写，
   CPython 与 AiPy 的输出必须逐字节相同。这是「实现的是 Python 子集」
   这句话的证据。
2. `03_builtins.py` 用了 AiPy 独有内建（`parse_number` 等），CPython 跑不了，
   所以只断言它能编译、能跑完、且关键行符合预期。
3. **每个示例都打包成 exe 实跑**：输出必须与脚本一致。这一步验的是尾部
   索引，字节码正确时它仍可能坏；而且 03 用了独有内建，会把「内建名表」
   那条路径也走一遍。

拿 CPython 当基准而不是写死期望输出：示例改了输出自动跟着变，不会出现
「代码对了、测试过期了」这种把新用户坑一遍的情况。
"""

import io
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))


def run_vm(pcc, aipy_run, script, out_dir):
    """编译脚本、用 AiPy 跑，返回 stdout。"""
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(script))[0]
    path = os.path.join(out_dir, stem + '.bc')
    compiled = subprocess.run([pcc, script, path], capture_output=True,
                              text=True, encoding='utf-8', errors='replace')
    if compiled.returncode != 0:
        raise SystemExit('%s 编译失败：%s'
                         % (os.path.basename(script), compiled.stderr.strip()))

    completed = subprocess.run([aipy_run, path], capture_output=True, text=True,
                               encoding='utf-8')
    if completed.returncode != 0:
        raise SystemExit('%s 执行失败：%s' % (os.path.basename(script),
                                            completed.stderr.strip()))
    return completed.stdout


def run_cpython(script):
    completed = subprocess.run([sys.executable, script], capture_output=True,
                               text=True, encoding='utf-8')
    if completed.returncode != 0:
        raise SystemExit('%s 在 CPython 里跑不起来：%s'
                         % (os.path.basename(script), completed.stderr.strip()))
    return completed.stdout


def compare_with_cpython(pcc, aipy_run, script, out_dir):
    """逐行比对：两个实现的输出必须完全一致。"""
    expected = run_cpython(script)
    actual = run_vm(pcc, aipy_run, script, out_dir)
    if expected == actual:
        return None

    name = os.path.basename(script)
    expected_lines = expected.splitlines()
    actual_lines = actual.splitlines()
    problems = ['%s 的输出与 CPython 不一致：' % name]
    for index in range(max(len(expected_lines), len(actual_lines))):
        want = expected_lines[index] if index < len(expected_lines) else '<没有这一行>'
        got = actual_lines[index] if index < len(actual_lines) else '<没有这一行>'
        if want != got:
            problems.append('  第 %d 行' % (index + 1))
            problems.append('    CPython: %r' % want)
            problems.append('    AiPy   : %r' % got)
    return problems


def check_only_aipy(pcc, aipy_run, script, out_dir, required):
    """CPython 跑不了（用了独有内建）时，只校验关键行存在。"""
    actual = run_vm(pcc, aipy_run, script, out_dir)
    missing = [line for line in required if line not in actual]
    if not missing:
        return None
    return ['%s 缺少预期输出：' % os.path.basename(script)] + \
           ['  未找到: %r' % line for line in missing]


def check_bundle(pcc, script, out_dir, build_dir=None, lib_dir=None):
    """打包成 exe：产物直接运行必须与脚本文本输出完全一致。

    ── 这一步在验什么 ──

    不是「字节码对不对」（前面几条已经验过了），而是**尾部索引**：

      * 追加位置算错 → 读到一段错位数据，加载失败
      * 长度字段写错 → 截断或多拉进 launcher 的字节
      * 用 `argv[0]` 找自己 → 从别的目录启动就读不到文件

    这三类错误都不会影响 `.bc` 文件本身，所以只能靠「真的跑一次 exe」发现。

    `build_dir` / `lib_dir` 告诉打包器去哪找构建配方与 `.pylib`。所有打包
    都走重链接（见 pcc 的 bundleMain），所以这两个参数是必需的。
    """
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(script))[0]
    app = os.path.join(out_dir, stem + '.exe')
    command = [pcc, '--exe', script, app]
    if build_dir:
        command += ['--build-dir', build_dir]
    if lib_dir:
        command += ['--lib-dir', lib_dir]
    completed = subprocess.run(command, capture_output=True, text=True,
                               encoding='utf-8')
    if completed.returncode != 0:
        return ['%s 打包失败：%s' % (stem, completed.stderr.strip())]

    # 从产物所在的目录启动：验证它靠 GetModuleFileName 而不是 argv[0]
    # 找自己（不同目录下的 argv[0] 形态不一样）。
    ran = subprocess.run([app], capture_output=True, text=True, encoding='utf-8',
                         cwd=out_dir)
    if ran.returncode != 0:
        return ['%s.exe 执行失败：%s' % (stem, ran.stderr.strip())]
    return ran


def check_bundle_matches(pcc, script, out_dir, expected, build_dir, lib_dir):
    """打包后的 exe 输出必须与给定期望一致。"""
    result = check_bundle(pcc, script, out_dir, build_dir, lib_dir)
    if isinstance(result, list):
        return result
    if result.stdout != expected:
        stem = os.path.splitext(os.path.basename(script))[0]
        problems = ['%s.exe 的输出与脚本不一致：' % stem]
        want = expected.splitlines()
        got = result.stdout.splitlines()
        for index in range(max(len(want), len(got))):
            a = want[index] if index < len(want) else '<没有这一行>'
            b = got[index] if index < len(got) else '<没有这一行>'
            if a != b:
                problems.append('  第 %d 行' % (index + 1))
                problems.append('    期望: %r' % a)
                problems.append('    实际: %r' % b)
        return problems
    return None


def main(argv):
    if len(argv) < 4:
        print('用法: check_examples.py <pcc.exe> <aipy_run.exe> '
              '<build目录> [lib目录]', file=sys.stderr)
        return 2
    pcc = argv[1]
    aipy_run = argv[2]
    build_dir = os.path.abspath(argv[3])
    lib_dir = os.path.abspath(argv[4]) if len(argv) > 4 else build_dir
    for path in (pcc, aipy_run):
        if not os.path.isfile(path):
            print('找不到 %s' % path, file=sys.stderr)
            return 2
    out_dir = os.path.join(os.environ.get('TEMP', '.'), 'aipy_examples')

    examples = os.path.join(AIPY, 'examples')
    failures = []

    # 与 CPython 逐字节比对的三份。期望值直接来自 CPython，不手写。
    cpython_examples = ('01_basics.py', '02_wordcount.py', '04_tool.py')
    for name in cpython_examples:
        script = os.path.join(examples, name)
        problems = compare_with_cpython(pcc, aipy_run, script, out_dir)
        if problems:
            failures.extend(problems)
        else:
            print('ok   %-17s 与 CPython 输出完全一致' % name)

    # 用 AiPy 独有内建的那份：只校验关键结果。
    builtins_script = os.path.join(examples, '03_builtins.py')
    required = [
        'parse_number(\'not a number\') = None',
        '坐标 (10, 20)',
        '1 + 2 = 3',
        '自检通过: 5 / 5',
        '注意 round(2.5) = 2 （银行家舍入）',
    ]
    problems = check_only_aipy(pcc, aipy_run, builtins_script, out_dir,
                               required)
    if problems:
        failures.extend(problems)
    else:
        print('ok   %-17s %d 条关键输出' % ('03_builtins.py', len(required)))

    # `threading` 在 CPython 里存在，但 AiPy 的 Thread 第一个参数是函数名
    # 而不是 target 函数，所以它不与 CPython 逐行比对、也不走 aipy_run
    # （aipy_run 没链这个库）——只用**打包后的 exe** 验证。这反而是更接近
    # 真实使用的路径。
    threads_script = os.path.join(examples, '05_threads.py')
    thread_required = [
        '启动前线程数: 1',
        '完成的任务数: 4',
        '汇总 TOTAL = 4950',
        '主线程仍能拿到锁',
        '线程名: MainThread',
    ]
    result = check_bundle(pcc, threads_script, out_dir,
                          build_dir=build_dir, lib_dir=lib_dir)
    if isinstance(result, list):
        failures.extend(result)
    else:
        missing = [line for line in thread_required if line not in result.stdout]
        if missing:
            failures.extend(['05_threads.exe 缺少预期输出：'] +
                            ['  未找到: %r' % line for line in missing])
            print('  ✗ 05_threads.exe 输出不符')
        else:
            print('ok   %-17s %d 条关键输出（打包产物）'
                  % ('05_threads.exe', len(thread_required)))

    # ── 每个示例都打包成 exe 实跑 ──
    #
    # 这是交付形态，只验字节码等价不够：尾部索引写错时字节码本身完全正确，
    # 坏的是「怎么读回来」。每份示例都要过，因为漏掉一个就可能漏掉只有
    # 那份脚本才会踩到的路径（比如 03 用了独有内建，内建名表要走一遍）。
    # 这几份：产物输出必须与 VM 跑脚本时一致。
    for name in cpython_examples + ('03_builtins.py',):
        script = os.path.join(examples, name)
        expected = run_cpython(script) if name in cpython_examples \
            else run_vm(pcc, aipy_run, script, out_dir)
        problems = check_bundle_matches(pcc, script, out_dir, expected,
                                       build_dir, lib_dir)
        if problems:
            failures.extend(problems)
        else:
            print('ok   %-17s 打包成 exe 后输出一致'
                  % (os.path.splitext(name)[0] + '.exe'))

    # 06 用 fs/time/json 三个系统库，会在**当前目录**建工作区（并自己清理）。
    # 所以给它一个空目录，并断言关键输出——其中「目录内容」依赖它自己
    # 建的两个文件，是这份脚本最值得验的一条。
    files_script = os.path.join(examples, '06_files.py')
    files_required = [
        '建目录: True',
        '记录数: 3',
        '内容: [\'ALPHA\', \'BETA\', \'GAMMA\']',
        'JSON: {"names":["ALPHA","BETA","GAMMA"],"count":3,"ok":true}',
        '还原 count: 3',
        '计时有效: True',
        'with 写完: True',
        '逐行读完共 8 字',
        "目录内容: ['lines.txt', 'records.txt']",
        '清理后存在: False',
    ]
    work = tempfile.mkdtemp(prefix='aipy_06_')
    result = check_bundle(pcc, files_script, work,
                          build_dir=build_dir, lib_dir=lib_dir)
    if isinstance(result, list):
        failures.extend(result)
    else:
        missing = [line for line in files_required if line not in result.stdout]
        if missing:
            failures.extend(['06_files.exe 缺少预期输出：'] +
                            ['  未找到: %r' % line for line in missing])
            print('  ✗ 06_files.exe 输出不符')
        else:
            print('ok   %-17s %d 条关键输出（打包产物）'
                  % ('06_files.exe', len(files_required)))

    if failures:
        for line in failures:
            print(line, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
