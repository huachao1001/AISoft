#!/usr/bin/env python3
"""`os.tray` / `os.fileassoc` 两个系统库的端到端验证。

── 验什么 ──

两份库都是**平台能力**（Windows 外壳），没有 CPython 对应物可以逐行对拍，
所以验证方式与 fs/time/json 不同：

1. **函数级按需**：只调一个函数时，库里其余函数的 obj 不进产物。这条只能
   靠「真的链接一次、再读产物里的注册节」来验——源码层面看不出来。
2. **失败路径的约定**：两个库都用返回值表示失败（False / 错误描述字符串），
   不用异常。这类约定最容易在改动中退化：某个函数改成返回 None，调用方的
   `if ok == False` 会静默失效。
3. **注册表只写 HKCU、且能干净撤销**：这是本库唯一的副作用，必须证明
   「写完能删干净」——否则用户卸载程序后系统里会留一辈子垃圾。

── 为什么不测「真的双击打开」 ──

那需要模拟资源管理器、等用户点确认（Win8 后的 UserChoice 会弹框），
在本机自动化里既不可靠也不是我们要保证的事。库的职责到「注册表值正确」
为止，而那一层上面直接查注册表就能验。

用法：check_os_shell.py <pcc.exe> <build 目录> <lib 目录>
"""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))

# 测试用的扩展名。刻意选不可能与真实文件类型撞车的名字。
ASSOC_EXT = '.aipyshelltest'
ASSOC_PROGID = 'AiPyShellTest.Doc'


def run(exe, args=None, cwd=None, timeout=60):
    """跑一个产物。stdin 给空串，避免 launcher 的「按回车退出」卡住自动化。"""
    try:
        completed = subprocess.run(
            [exe] + (args or []), input='\n', capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=timeout,
            cwd=cwd or os.path.dirname(exe))
    except subprocess.TimeoutExpired:
        return -1, '', 'timeout'
    return completed.returncode, completed.stdout, completed.stderr


def linked_functions(pcc, exe):
    """读产物 `.aipyfn` 节里真实的扩展函数清单。"""
    completed = subprocess.run([pcc, '--linked-info', exe],
                               capture_output=True, text=True, encoding='utf-8')
    if completed.returncode != 0:
        raise SystemExit('读不到 %s 的注册节：%s'
                         % (os.path.basename(exe), completed.stderr.strip()))
    payload = json.loads('{' + completed.stdout + '}')
    return [(entry[0], entry[1]) for entry in payload['functions']]


def build(pcc, script_text, name, build_dir, lib_dir):
    """把脚本打成 exe，返回产物路径。"""
    folder = tempfile.mkdtemp(prefix='aipy_shell_')
    source = os.path.join(folder, name + '.py')
    with open(source, 'w', encoding='utf-8') as handle:
        handle.write(script_text)
    exe = os.path.join(folder, name + '.exe')
    completed = subprocess.run(
        [pcc, '--exe', source, exe,
         '--build-dir', build_dir, '--lib-dir', lib_dir],
        capture_output=True, text=True, encoding='utf-8')
    if completed.returncode != 0:
        raise SystemExit('打包 %s 失败：%s' % (name, completed.stderr.strip()))
    return exe


FAILURES = []


def check(label, ok, detail=''):
    if ok:
        print('  ✓ %s' % label)
    else:
        FAILURES.append('%s：%s' % (label, detail))
        print('  ✗ %s  %s' % (label, detail))


# ── fileassoc ──

# 只查不写：这个产物里不该出现 register/unregister/icon 的代码。
ASSOC_QUERY_ONLY = '''import os.fileassoc

print("query:" + repr(os.fileassoc.query("%s")))
print("owned:" + str(os.fileassoc.owned("%s")))
''' % (ASSOC_EXT, ASSOC_EXT)

# 完整往返：写入 → 查询 → 归属 → 撤销 → 确认清干净。
ASSOC_ROUNDTRIP = '''import os.fileassoc

EXT = "%s"
PROGID = "%s"

error = os.fileassoc.register(EXT, PROGID)
print("register:" + repr(error))
print("query:" + repr(os.fileassoc.query(EXT)))
print("owned:" + str(os.fileassoc.owned(EXT)))

# 图标写空路径 = 退回 exe 自带图标。
print("icon:" + repr(os.fileassoc.icon(EXT, "")))

print("unregister:" + repr(os.fileassoc.unregister(EXT)))
print("query_after:" + repr(os.fileassoc.query(EXT)))
print("owned_after:" + str(os.fileassoc.owned(EXT)))

# 无效扩展名必须在写注册表之前就被拒绝。
print("empty:" + str(os.fileassoc.register("") != ""))
print("spaces:" + str(os.fileassoc.register("a b") != ""))
print("dotonly:" + str(os.fileassoc.register(".") != ""))
''' % (ASSOC_EXT, ASSOC_PROGID)

# 参数通道：被双击时系统把文件路径作为第一个参数传进来。
ASSOC_ARGV = '''import sys

args = sys.argv()
print("argc:" + str(len(args)))
print("exe_ok:" + str(args[0].endswith(".exe")))
print("arg1:" + (args[1] if len(args) > 1 else ""))
'''

# ── tray ──

# 只配不跑：不该把消息循环拖进产物。
TRAY_CONFIGURE_ONLY = '''import os.tray

print("icon:" + str(os.tray.icon("", "probe")))
print("item:" + str(os.tray.item("open", "Open")))
print("separator:" + str(os.tray.separator()))
print("bad_on:" + str(os.tray.on("open", "not a function")))


def handler():
    print("ran")


print("on:" + str(os.tray.on("open", handler)))
print("on_click:" + str(os.tray.on("click", handler)))

# 没进消息循环之前，气泡没有依附的图标，必须明确返回 False。
print("notify:" + str(os.tray.notify("t", "x")))
'''


def check_fileassoc(pcc, build_dir, lib_dir):
    print('── os.fileassoc：注册表关联 ──')

    exe = build(pcc, ASSOC_QUERY_ONLY, 'assoc_query', build_dir, lib_dir)
    functions = linked_functions(pcc, exe)
    names = {name for module, name in functions if module == 'os.fileassoc'}
    check('只查时只链 query/owned（%s）' % sorted(names),
          names == {'query', 'owned'}, str(sorted(names)))

    status, out, err = run(exe)
    check('只查的产物能跑', status == 0, err.strip())
    check('未注册的扩展名查询为空串', "query:''" in out, out)
    check('未注册的扩展名不算本程序', 'owned:False' in out, out)

    exe = build(pcc, ASSOC_ROUNDTRIP, 'assoc_roundtrip', build_dir, lib_dir)
    status, out, err = run(exe)
    check('往返产物能跑', status == 0, err.strip())
    check('注册成功（返回空串）', "register:''" in out, out)
    check('查询到刚写的 ProgID', 'query:%r' % ASSOC_PROGID in out, out)
    check('关联被判为本程序', 'owned:True' in out, out)
    check('设置图标成功', "icon:''" in out, out)
    check('撤销成功', "unregister:''" in out, out)
    check('撤销后查询为空', "query_after:''" in out, out)
    check('撤销后不再归属本程序', 'owned_after:False' in out, out)
    check('空扩展名被拒绝', 'empty:True' in out, out)
    check('含空格的扩展名被拒绝', 'spaces:True' in out, out)
    check('只有一个点被拒绝', 'dotonly:True' in out, out)

    # 注册表必须真的干净：脚本自己撤销了，这里再确认一次。
    completed = subprocess.run(
        ['reg', 'query', 'HKCU\\Software\\Classes\\' + ASSOC_EXT],
        capture_output=True, text=True)
    check('注册表里没有残留的扩展名键',
          completed.returncode != 0, completed.stdout.strip())

    exe = build(pcc, ASSOC_ARGV, 'assoc_argv', build_dir, lib_dir)
    arguments = ['C:\\some dir\\sample' + ASSOC_EXT]
    status, out, err = run(exe, arguments)
    check('argv 产物能跑', status == 0, err.strip())
    check('攒到两个参数（exe + 文件）', 'argc:2' in out, out)
    check('argv[0] 是 exe 路径', 'exe_ok:True' in out, out)
    check('带空格的路径完整传进来',
          'arg1:C:\\some dir\\sample' + ASSOC_EXT in out, out)


def check_tray(pcc, build_dir, lib_dir):
    print('── os.tray：托盘配置 ──')

    exe = build(pcc, TRAY_CONFIGURE_ONLY, 'tray_configure',
                build_dir, lib_dir)
    functions = linked_functions(pcc, exe)
    names = {name for module, name in functions if module == 'os.tray'}
    check('不调 run 时没有链接 run（%s）' % sorted(names),
          'run' not in names and 'quit' not in names, str(sorted(names)))

    status, out, err = run(exe)
    check('托盘配置产物能跑', status == 0, err.strip())
    check('设置图标成功', 'icon:True' in out, out)
    check('加菜单项成功', 'item:True' in out, out)
    check('加分隔线返回 None', 'separator:None' in out, out)
    check('非函数值被拒绝', 'bad_on:False' in out, out)
    check('注册函数回调成功', 'on:True' in out, out)
    check('注册左键回调成功', 'on_click:True' in out, out)
    check('没进消息循环时气泡返回 False', 'notify:False' in out, out)
    check('拒绝非函数时给出诊断',
          'is not a function' in err, err.strip())

    # ── 消息循环真的能挂上、能退出 ──
    #
    # 这条只能实跑：`Shell_NotifyIcon` 失败、窗口类注册失败、退出路径漏摘
    # 图标，都只在真的进一次消息循环时才暴露。
    TRAY_RUN = '''import os.tray
import threading
import time

os.tray.icon("", "AiPy tray test")
os.tray.item("quit", "Quit")


def shutdown():
    os.tray.quit()


os.tray.on("quit", shutdown)


def closer():
    time.sleep(1.0)
    os.tray.quit()


threading.Thread("closer").start()
print("run:" + str(os.tray.run()))
'''
    exe = build(pcc, TRAY_RUN, 'tray_run', build_dir, lib_dir)
    functions = linked_functions(pcc, exe)
    names = {name for module, name in functions if module == 'os.tray'}
    check('调 run 时链上 run/quit（%s）' % sorted(names),
          {'run', 'quit'} <= names, str(sorted(names)))

    # 消息循环本身要能自己退出（后台线程 1 秒后调 quit），所以这里给足
    # 超时即可；stdin 给空串是为了 launcher 的「按回车」不卡住。
    status, out, err = run(exe, timeout=30)
    check('消息循环正常起停', status == 0, err.strip())
    check('run() 返回 True', 'run:True' in out, out)


def main(argv):
    if len(argv) < 4:
        print('用法: check_os_shell.py <pcc.exe> <build 目录> <lib 目录>',
              file=sys.stderr)
        return 2
    pcc, build_dir, lib_dir = argv[1], argv[2], argv[3]

    print('── 前置：两个库都真的打包出来了 ──')
    for module in ('os.fileassoc', 'os.tray'):
        archive = os.path.join(lib_dir, module + '.pylib')
        check('%s.pylib 存在' % module, os.path.isfile(archive), archive)

    check_fileassoc(pcc, build_dir, lib_dir)
    check_tray(pcc, build_dir, lib_dir)

    print()
    if FAILURES:
        print('%d 项失败：' % len(FAILURES))
        for item in FAILURES:
            print('  - ' + item)
        return 1
    print('全部通过')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
