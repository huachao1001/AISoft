#!/usr/bin/env python3
"""按平台分派的链接步骤：把「哪些是平台细节」收在一个文件里。

── 为什么单独一个文件 ──

`link.py` 的职责是「按扩展表决定链什么」——那是语言机制，与操作系统无关。
真正与平台绑定的只有三件事：

  1. 用哪个链接器、参数长什么样（MSVC: `link.exe /INCLUDE:`；GNU: `ld -u`）
  2. 静态库的扩展名（`.lib` / `.a`）
  3. 默认要带的系统库（`kernel32.lib` … / `-lc -lpthread`）

把它们抽出来，是为了将来加一个平台时**只改本文件**，不动 link.py 的
按需逻辑，也不动 VM。当前只实现了 Windows/MSVC——这不是懒，是唯一有
真实验证路径的那一个（CI、测试、用户环境都是 Windows）。别的平台留出
接口而不是半成品代码：半成品会被误以为能用。

── 接口长什么样 ──

每个平台实现一个 `Toolchain`：
  library_suffix      静态库扩展名
  symbol_for(module, function)   函数级注册符号名
  module_symbol(module)          模块级注册符号名
  quote_include(symbol)          「强制把某个符号拉进来」的参数
  whole_archive(path)            「整个库都进来」的参数（兜底策略）
  library_names(paths)           读库里的外部符号名（打包期裁剪要用）
"""

import os
import subprocess


class ToolchainError(Exception):
    """平台层的问题，直接给命令行用户看。"""


class MsvcToolchain(object):
    """Windows / MSVC。

    为什么断言 MSVC 而不是「Windows 通用」：符号命名（`aipy_<模块>_fn_<函数>`）
    和节合并（`.aipyfn$k`）都是本项目的 C++ 侧实现决定的，而那一侧目前
    只有 MSVC 分支（见 include/aipy/pylib.h）。换个编译器连 .lib 都产不出来。
    """

    name = 'msvc'
    library_suffix = '.lib'

    def __init__(self, link_exe, library_paths):
        self.link_exe = link_exe
        self.library_paths = list(library_paths or [])

    def module_symbol(self, module):
        return 'aipy_pylib_entry_%s' % module

    def symbol_for(self, module, function):
        return 'aipy_%s_fn_%s' % (module, function)

    def quote_include(self, symbol):
        return '/INCLUDE:' + symbol

    def whole_archive(self, path):
        return '/WHOLEARCHIVE:' + path

    def output_flag(self, path):
        return '/OUT:' + path

    def library_names(self, path):
        """读 .lib 里的外部符号名。

        用 `link.exe /dump /symbols` 而不是 `dumpbin`：用户机器上只装链接器
        （约 15 MB 的 kit），不该额外要求一个 SDK 工具。
        """
        env = dict(os.environ)
        if self.library_paths:
            inherited = env.get('LIB')
            env['LIB'] = ';'.join(self.library_paths) + (
                ';' + inherited if inherited else '')

        completed = subprocess.run([self.link_exe, '/dump', '/symbols', path],
                                   capture_output=True, text=True,
                                   encoding='utf-8', errors='replace', env=env)
        if completed.returncode != 0:
            return set()
        names = set()
        for line in completed.stdout.splitlines():
            # 形如 `00F 00000000 SECT5  notype  External  | aipy_text_fn_upper`
            if '|' not in line:
                continue
            name = line.rsplit('|', 1)[1].strip()
            if name:
                names.add(name)
        return names


# 已实现的平台。加一个操作系统就是往这里加一项，并实现一个 Toolchain。
_TOOLCHAINS = {
    'msvc': MsvcToolchain,
}


def for_current_host(link_exe, library_paths=None):
    """按当前平台给出 Toolchain。

    判据用 `os.name`/`sys.platform` 而不是「链接器路径里有没有 .exe」：
    后者在交叉编译时会把宿主编译器误判成本地平台。
    """
    import sys
    if sys.platform.startswith('win'):
        return MsvcToolchain(link_exe, library_paths)
    raise ToolchainError(
        '当前平台 %s 还没有实现 pylib 链接（只实现了 Windows/MSVC）。'
        '要加一个平台，实现 tools/aipyic/toolchain.py 里的 Toolchain，'
        '并补上 include/aipy/pylib.h 的对应分支。' % sys.platform)
