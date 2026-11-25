#!/usr/bin/env python3
"""dist/ 的发布契约：只有 `.pylib`，而且只用它就能打包。

── 为什么需要这条测试 ──

`dist/` 是「拿到它就能用」的目录。它的形态一旦松掉，症状不会出现在任何
功能测试里：

  * `dist/lib` 里多一个裸 `.lib` —— 谁都不知道哪个必须、哪个能删
  * 运行时散放在 `lib/` —— 「运行时是不是库」这件事没有答案
  * 测试夹具混进发布清单 —— 用户以为 `text` 是标准库

所以这里的判据不是「功能对不对」，而是「目录里有什么」。它读的是**真的
dist/ 目录**（不是构建目录），所以能挡住「构建能跑、dist 不能跑」。

── 为什么还要实跑一遍 ──

只检查文件清单挡不住「文件都在但解不出来」。`core.pylib` 里的三个库如果
少一个、或者角色名写错，文件清单仍然全绿，而打包会失败。所以拷到临时目录
实打实打包 + 运行一次。

用法：
  check_dist.py <dist目录> <期望的平台键>
"""

import io
import os
import shutil
import subprocess
import sys
import tempfile


SCRIPT = '''import os
import os.path
import json

os.makedirs("out")
with open("out/a.txt", "w") as handle:
    handle.write("alpha\\nbeta\\n")

print("exists", os.path.exists("out/a.txt"))
print("size", os.path.getsize("out/a.txt"))

lines = []
with open("out/a.txt") as handle:
    for line in handle:
        lines.append(line.strip())
print("lines", lines)

print("json", json.dumps(lines))
print("listdir", os.listdir("out"))
print("remove", os.remove("out/a.txt"))
'''

EXPECTED = ('exists True\nsize 13\nlines [\'alpha\', \'beta\']\n'
            'json ["alpha","beta"]\nlistdir [\'a.txt\']\nremove True\n')


def run(argv, **kwargs):
    return subprocess.run(argv, capture_output=True, text=True,
                          encoding='utf-8', errors='replace', **kwargs)


def required_archives(platform):
    """发布目录里必须有的归档，以及必须**没有**的东西。"""
    # 扩展库：CPython 标准库里的名字。
    archives = ['core', 'fs', 'json', 'os', 'os.path', 'threading', 'time']
    # `text` 是 tests/pylib 里的测试夹具，不该出现在发布目录。
    forbidden = ['text']
    return archives, forbidden


def check_layout(dist, failures):
    print('=== 目录形状 ===')
    libdir = os.path.join(dist, 'lib')
    if not os.path.isdir(libdir):
        failures.append('缺少 lib/ 目录')
        return

    names = sorted(os.listdir(libdir))

    # 1. 不允许裸 .lib / .a：运行时封在 core.pylib 里，扩展库只有 .pylib。
    stray = [n for n in names if n.endswith('.lib') or n.endswith('.a')]
    if stray:
        failures.append('lib/ 里有裸原生库：%s'
                        % ', '.join(stray))
        print('  ✗ 裸原生库：%s' % ', '.join(stray))
    else:
        print('  ✓ lib/ 里没有裸 .lib / .a')

    # 2. 必需的归档一个都不能少。
    missing = [m for m in required_archives('win-x64')[0]
               if (m + '.pylib') not in names]
    if missing:
        failures.append('缺少归档：%s' % ', '.join(m + '.pylib' for m in missing))
        print('  ✗ 缺少归档：%s' % ', '.join(missing))
    else:
        print('  ✓ 归档齐全（%d 个）' % len(names))

    # 3. 测试夹具不许混进来。
    for module in required_archives('win-x64')[1]:
        if (module + '.pylib') in names:
            failures.append('%s.pylib 是测试夹具，不该进发布目录' % module)
            print('  ✗ %s.pylib 混进了 dist' % module)
    else:
        print('  ✓ 没有测试夹具')

    # 4. 根目录只有编译器。
    expected_root = {'pcc.exe', 'lib'}
    actual_root = set(os.listdir(dist))
    extra = actual_root - expected_root
    if extra:
        failures.append('根目录有多余文件：%s' % ', '.join(sorted(extra)))
        print('  ✗ 根目录多余：%s' % ', '.join(sorted(extra)))
    else:
        print('  ✓ 根目录只有编译器与 lib/')


def check_core_archive(dist, failures):
    """core.pylib 必须带齐三个角色，且每个都能解出来。"""
    print()
    print('=== core.pylib 的角色 ===')
    icc = os.path.join(dist, 'pcc.exe')
    result = run([icc, '--pylib-info', os.path.join(dist, 'lib', 'core.pylib')])
    if result.returncode != 0:
        failures.append('读 core.pylib 失败：%s' % result.stderr.strip())
        print('  ✗ %s' % result.stderr.strip())
        return

    text = result.stdout
    for role in ('runtime', 'base', 'launcher'):
        if role not in text:
            failures.append('core.pylib 里没有 %s 角色' % role)
            print('  ✗ 缺少角色 %s' % role)
        else:
            print('  ✓ 角色 %s' % role)


def check_dist_only_usage(dist, failures):
    """拷到临时目录，只用 dist 打包并运行。"""
    print()
    print('=== 只用 dist 打包 + 运行 ===')
    work = tempfile.mkdtemp(prefix='aipy_dist_')
    try:
        for name in os.listdir(dist):
            source = os.path.join(dist, name)
            target = os.path.join(work, name)
            if os.path.isdir(source):
                shutil.copytree(source, target)
            else:
                shutil.copy2(source, target)

        script = os.path.join(work, 'app.py')
        with io.open(script, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(SCRIPT)

        exe = os.path.join(work, 'app.exe')
        built = run([os.path.join(work, 'pcc.exe'), '--exe', script, exe,
                     '--lib-dir', work])
        if built.returncode != 0:
            failures.append('只用 dist 打包失败：%s'
                            % (built.stderr.strip() or built.stdout.strip()))
            print('  ✗ 打包失败')
            return
        print('  ✓ 打包成功（%d bytes）' % os.path.getsize(exe))

        ran = run([exe], cwd=work)
        if ran.returncode != 0:
            failures.append('产物运行失败：%s' % ran.stderr.strip())
            print('  ✗ 运行失败')
            return
        if ran.stdout != EXPECTED:
            failures.append('产物输出与期望不符：\n实际 %r\n期望 %r'
                            % (ran.stdout, EXPECTED))
            print('  ✗ 输出不符')
            return
        print('  ✓ 输出与期望逐行一致')
    finally:
        shutil.rmtree(work, ignore_errors=True)


def check_double_click_behavior(dist, failures):
    """双击控制台程序的窗口不能一闪而逝；从 cmd 调用不能被卡住。

    这两条是同一个判据的正反两面，`GetConsoleProcessList` 区分它们：
      1 个进程 → 独占控制台（双击），结尾等回车
      2+ 个进程 → 共用控制台（cmd 里调用），立刻返回

    只测「双击会等」而不测「cmd 不卡」，等于允许把批处理卡死的回归通过。
    """
    print()
    print('=== 双击与命令行调用 ===')
    work = tempfile.mkdtemp(prefix='aipy_hold_')
    try:
        for name in os.listdir(dist):
            source = os.path.join(dist, name)
            target = os.path.join(work, name)
            if os.path.isdir(source):
                shutil.copytree(source, target)
            else:
                shutil.copy2(source, target)

        script = os.path.join(work, 'hold.py')
        with io.open(script, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write('print("hold probe")\n')
        exe = os.path.join(work, 'hold.exe')
        built = run([os.path.join(work, 'pcc.exe'), '--exe', script, exe,
                     '--lib-dir', work])
        if built.returncode != 0:
            failures.append('双击行为用例打包失败：%s'
                            % (built.stderr.strip() or built.stdout.strip()))
            print('  ✗ 打包失败')
            return

        # A. 从本进程调用：共用控制台，必须立刻返回。
        try:
            done = run([exe], cwd=work, timeout=20)
            if done.stdout.strip() == 'hold probe':
                print('  ✓ 命令行调用：立刻返回')
            else:
                failures.append('命令行调用输出不对：%r' % done.stdout.strip())
                print('  ✗ 命令行调用输出不对')
        except subprocess.TimeoutExpired:
            failures.append('命令行调用卡住了（不该等回车）')
            print('  ✗ 命令行调用卡住')

        # B. 新建独占控制台（等价于双击）：应当停在等待。
        # 超时说明它在等回车——那正是要的行为，所以这里超时算通过。
        if os.name != 'nt':
            print('  - 跳过独占控制台检查（非 Windows）')
            return
        started = subprocess.Popen(['cmd', '/c', 'start', '/wait', exe],
                                   cwd=work, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        waiting = False
        try:
            started.wait(timeout=6)
        except subprocess.TimeoutExpired:
            waiting = True
        finally:
            if started.poll() is None:
                subprocess.run(['taskkill', '/F', '/T', '/PID',
                                str(started.pid)], capture_output=True)
        if waiting:
            print('  ✓ 双击（独占控制台）：停在等待，窗口不会闪退')
        else:
            failures.append('双击控制台程序后立刻退出，窗口会闪退')
            print('  ✗ 双击后立刻退出')
    finally:
        shutil.rmtree(work, ignore_errors=True)


def check_no_developer_prompt(dist, failures):
    """不跑 vcvars 也得能打包。

    这是「dist 拷到任何机器都能用」的实际判据。拿到 dist 的人不会先执行
    `vcvars64.bat`——如果 pcc 只能靠它设好的 PATH 找到 link.exe，那句话
    就是假的。

    PATH 收到只剩系统目录、清掉 INCLUDE/LIB/VCINSTALLDIR，再看能不能打包。
    链接器应当由 vswhere 找到（它按安装记录查，与 PATH 无关）。
    """
    print()
    print('=== 不依赖 vcvars 的 PATH ===')
    work = tempfile.mkdtemp(prefix='aipy_nopath_')
    try:
        for name in os.listdir(dist):
            source = os.path.join(dist, name)
            target = os.path.join(work, name)
            if os.path.isdir(source):
                shutil.copytree(source, target)
            else:
                shutil.copy2(source, target)

        env = os.environ.copy()
        env['PATH'] = 'C:\\Windows\\system32;C:\\Windows'
        for key in ('INCLUDE', 'LIB', 'LIBPATH', 'VCINSTALLDIR', 'VSINSTALLDIR'):
            env.pop(key, None)

        script = os.path.join(work, 'nopath.py')
        with io.open(script, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write('print("nopath ok")\n')
        exe = os.path.join(work, 'nopath.exe')

        built = run([os.path.join(work, 'pcc.exe'), '--exe', script, exe,
                     '--lib-dir', work], env=env)
        if built.returncode != 0:
            failures.append(
                '清掉 vcvars 的环境后打包失败（链接器没找到）：%s'
                % (built.stderr.strip() or built.stdout.strip())[:300])
            print('  ✗ 打包失败')
            return

        ran = run([exe], cwd=work, env=env)
        if ran.stdout.strip() != 'nopath ok':
            failures.append('无 vcvars 产物运行输出不对：%r' % ran.stdout.strip())
            print('  ✗ 输出：%r' % ran.stdout.strip())
            return
        print('  ✓ 只用系统 PATH 也能找到链接器并跑通')
    finally:
        shutil.rmtree(work, ignore_errors=True)


def check_missing_core(dist, failures):
    """移走 core.pylib：报错必须点名它，而不是含糊的链接失败。"""
    print()
    print('=== core.pylib 缺失时的报错 ===')
    work = tempfile.mkdtemp(prefix='aipy_nocore_')
    try:
        for name in os.listdir(dist):
            source = os.path.join(dist, name)
            target = os.path.join(work, name)
            if os.path.isdir(source):
                shutil.copytree(source, target)
            else:
                shutil.copy2(source, target)

        # 拿走 core.pylib 后这个目录不再算「dist」：pcc 判据是 lib/core.pylib
        # 在不在，于是它要求 --build-dir。报错必须说清缺什么，而不是含糊地
        # 报一个链接失败。
        core = os.path.join(work, 'lib', 'core.pylib')
        hidden = core + '.hidden'
        os.rename(core, hidden)
        try:
            script = os.path.join(work, 'app.py')
            with io.open(script, 'w', encoding='utf-8', newline='\n') as handle:
                handle.write('import os\nprint(os.path.exists("."))\n')
            built = run([os.path.join(work, 'pcc.exe'), '--exe', script,
                         os.path.join(work, 'app.exe'), '--lib-dir', work])
        finally:
            os.rename(hidden, core)

        message = built.stderr.strip() or built.stdout.strip()
        if built.returncode == 0:
            failures.append('core.pylib 缺失时仍然打包成功')
            print('  ✗ 没有报错')
        elif 'core.pylib' not in message and '--build-dir' not in message:
            failures.append('报错没说清缺什么：%s' % message)
            print('  ✗ 报错含糊：%s' % message[:200])
        else:
            print('  ✓ 报错说清了缺什么')
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main(argv):
    if len(argv) != 2:
        sys.stderr.write('usage: check_dist.py <dist目录>\n')
        return 2

    dist = os.path.abspath(argv[1])
    if not os.path.isdir(dist):
        sys.stderr.write('不是目录：%s\n' % dist)
        return 2

    failures = []
    check_layout(dist, failures)
    check_core_archive(dist, failures)
    check_dist_only_usage(dist, failures)
    check_double_click_behavior(dist, failures)
    check_no_developer_prompt(dist, failures)
    check_missing_core(dist, failures)

    print()
    if failures:
        print('失败 %d 项：' % len(failures))
        for item in failures:
            print('  - %s' % item)
        return 1
    print('dist/ 契约全部满足')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
