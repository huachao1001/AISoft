#!/usr/bin/env python3
"""守住 Design.md 第 4 节：跨平台预留 + 库扩展标准化。

── 为什么要单独一份测试 ──

这两条都是「写起来没问题、退化时也没人发现」的那类要求：

1. **标准化**：只要有人图方便，在某个库里重新写一份私有 registry.h，
   功能照样跑，但「系统库与用户自定义库同一套标准」就不成立了。判据是
   「两个库的注册行长得一样」+「没有私有注册头」。
2. **跨平台**：Windows 上的代码写死 `link.exe`/`kernel32` 也能跑，但那些
   字符串散落在 link.py 里就意味着「加一个平台要改核心逻辑」。判据是
   「平台细节集中在 toolchain.py」，且「不支持平台报清晰的错而不是崩溃」。

两条都只能静态检查（跨平台没法在当前机器上真的跑），所以这里做的是
结构断言而不是行为断言。

用法：check_standard.py <源码根目录> <python>
"""

import io
import os
import sys

FAILURES = []


def check(label, ok, detail=''):
    if ok:
        print('  ✓ %s' % label)
    else:
        FAILURES.append('%s：%s' % (label, detail))
        print('  ✗ %s  %s' % (label, detail))


def read(path):
    return io.open(path, encoding='utf-8', errors='replace').read()


def macro_lines(path):
    return [line.strip() for line in read(path).splitlines()
            if line.strip().startswith('AIPY_')]


def main(argv):
    if len(argv) < 2:
        print('用法: check_standard.py <源码根目录>', file=sys.stderr)
        return 2
    root = os.path.abspath(argv[1])

    print('── 4.2 库扩展标准化：系统库与用户库用同一套宏 ──')

    sys_lib = macro_lines(os.path.join(root, 'libs/threading/src/fn_thread.cpp'))
    user_lib = macro_lines(os.path.join(root, 'tests/pylib/fn_upper.cpp'))
    check('系统库用 AIPY_PYLIB_FUNCTION',
          any(l.startswith('AIPY_PYLIB_FUNCTION(threading,') for l in sys_lib),
          str(sys_lib))
    check('用户库用同一个宏（只差模块名）',
          any(l.startswith('AIPY_PYLIB_FUNCTION(text,') for l in user_lib),
          str(user_lib))

    sys_module = macro_lines(os.path.join(root, 'libs/threading/src/module.cpp'))
    user_module = macro_lines(os.path.join(root, 'tests/pylib/text_pylib.cpp'))
    check('两边都用 AIPY_PYLIB_MODULE 声明模块',
          any(l.startswith('AIPY_PYLIB_MODULE(') for l in sys_module) and
          any(l.startswith('AIPY_PYLIB_MODULE(') for l in user_module),
          'system=%s user=%s' % (sys_module, user_module))

    # 私有注册机制必须不存在：有它就是两个标准。
    private = []
    for base, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in ('build', 'dist', '.git',
                                                '__pycache__')]
        for name in files:
            if 'registry' in name and name.endswith(('.h', '.cpp')):
                private.append(os.path.relpath(os.path.join(base, name), root))
    check('没有各库私有的 registry.h / registry.cpp', not private, str(private))

    # 旧宏名不该再出现（它是 threading 专用的历史形态）。
    legacy = []
    for base, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in ('build', 'dist', '.git',
                                                '__pycache__')]
        for name in files:
            if not name.endswith(('.h', '.cpp')):
                continue
            path = os.path.join(base, name)
            if 'AIPY_THREADING_FN' in read(path):
                legacy.append(os.path.relpath(path, root))
    check('旧的 AIPY_THREADING_FN 已全部迁走', not legacy, str(legacy))

    print()
    print('── 4.1 跨平台：平台细节集中在 toolchain.py ──')

    toolchain = os.path.join(root, 'tools/aipyic/toolchain.py')
    check('存在 tools/aipyic/toolchain.py', os.path.isfile(toolchain))

    link_py = read(os.path.join(root, 'tools/aipyic/link.py'))
    # link.py 里不该再有写死的平台参数；它们属于 toolchain。
    platform_tokens = ["'/INCLUDE:", "'/WHOLEARCHIVE:", "advapi32", "uuid.lib"]
    found = [token for token in platform_tokens if token in link_py]
    check('link.py 里没有写死的链接器参数', not found, str(found))

    check('link.py 通过 for_current_host 取平台实现',
          'for_current_host' in link_py)

    # 不支持的平台必须报清晰错误，而不是抛 NameError 或静默返回空。
    sys.path.insert(0, os.path.join(root, 'tools', 'aipyic'))
    import toolchain as module
    try:
        original = sys.platform
        sys.platform = 'linux'
        try:
            module.for_current_host('ld')
            raised = None
        except module.ToolchainError as error:
            raised = str(error)
        finally:
            sys.platform = original
        check('不支持的平台报 ToolchainError（而不是崩溃）',
              raised is not None and '还没有实现' in raised, str(raised))
    except ImportError as error:
        check('能 import toolchain', False, str(error))

    print()
    if FAILURES:
        print('失败：', file=sys.stderr)
        for line in FAILURES:
            print('  ' + line, file=sys.stderr)
        return 1
    print('Design.md 第 4 节（跨平台 + 标准化）全部满足。')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
