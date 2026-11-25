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

    # 所有系统库都必须用同一套宏。这条是「标准化」的实际判据：加一个新库
    # 时不复制任何私有注册代码，只写 AIPY_PYLIB_MODULE + 若干 FUNCTION。
    #
    # libs/io 是例外：它不是能力，是 fs / os / os.path 共用的路径与换行
    # 工具层，没有注册项也不导出任何脚本函数。所以不要求 module.cpp。
    shared_layers = {'io'}

    system_libraries = []
    libs_root = os.path.join(root, 'libs')
    if os.path.isdir(libs_root):
        system_libraries = sorted(
            name for name in os.listdir(libs_root)
            if os.path.isdir(os.path.join(libs_root, name))
            and name not in shared_layers)

    check('libs/ 下有多个系统库（至少 thread/fs/time/json 四个）',
          len(system_libraries) >= 4, str(system_libraries))

    for name in system_libraries:
        module_cpp = os.path.join(libs_root, name, 'src', 'module.cpp')
        if not os.path.isfile(module_cpp):
            check('%s 有 module.cpp' % name, False, '缺失')
            continue
        declared = macro_lines(module_cpp)
        # 点分模块名（`os.path`）用 _NAMED 变体：符号前缀是 os_path，
        # 对外名字带点。两种都算「用了标准宏」。
        ok = any(l.startswith('AIPY_PYLIB_MODULE(%s)' % name) or
                 l.startswith('AIPY_PYLIB_MODULE_NAMED(%s,' % name)
                 for l in declared)
        check('%s 用 AIPY_PYLIB_MODULE(%s)' % (name, name), ok, str(declared))

        # 每个 fn_*.cpp 必须用 FUNCTION 宏，且模块名对得上。
        src_dir = os.path.join(libs_root, name, 'src')
        bad = []
        produced = 0
        for file_name in sorted(os.listdir(src_dir)):
            if not file_name.startswith('fn_') or not file_name.endswith('.cpp'):
                continue
            lines = macro_lines(os.path.join(src_dir, file_name))
            functions = [l for l in lines
                         if l.startswith('AIPY_PYLIB_FUNCTION(') or
                         l.startswith('AIPY_PYLIB_FUNCTION_NAMED(')]
            if not functions:
                bad.append('%s 没有 AIPY_PYLIB_FUNCTION' % file_name)
                continue
            produced += len(functions)
            for line in functions:
                if not (line.startswith('AIPY_PYLIB_FUNCTION(%s,' % name) or
                        line.startswith('AIPY_PYLIB_FUNCTION_NAMED(%s,' % name)):
                    bad.append('%s 的模块名不是 %s' % (file_name, name))
        check('%s 的导出函数都用 FUNCTION 宏（%d 个）' % (name, produced),
              not bad and produced > 0, str(bad))

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
    print('── 4.1 跨平台：平台细节集中在 toolchain ──')

    # 平台细节的唯一落点。Python 版（tools/aipyic）已降级为测试 oracle，
    # 发布件是 tools/pcc 里的 C++ 实现，所以这条断言必须盯着后者——
    # 盯 oracle 等于保护一个用户根本不会运行的文件。
    toolchain_cpp = os.path.join(root, 'tools/pcc/src/backend/toolchain.cpp')
    toolchain_h = os.path.join(root, 'tools/pcc/include/pcc/backend/toolchain.h')
    check('存在 tools/pcc 的 toolchain 实现',
          os.path.isfile(toolchain_cpp) and os.path.isfile(toolchain_h))

    # link 里不该再有写死的链接器参数；它们属于 toolchain。
    link_cpp = read(os.path.join(root, 'tools/pcc/src/backend/link.cpp'))
    platform_tokens = ['"/INCLUDE:', '"/WHOLEARCHIVE:', 'advapi32', 'uuid.lib']
    found = [token for token in platform_tokens if token in link_cpp]
    check('link.cpp 里没有写死的链接器参数', not found, str(found))

    check('link.cpp 通过 makeToolchain 取平台实现',
          'makeToolchain' in link_cpp)

    # 不支持的平台必须报清晰错误，而不是返回一个半成品实现。
    toolchain_source = read(toolchain_cpp)
    check('不支持的平台走 unsupportedPlatform 分支',
          'unsupportedPlatform' in toolchain_source)

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
