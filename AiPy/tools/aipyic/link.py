#!/usr/bin/env python3
"""按需链接 pylib：把用到的 `.lib` 与 launcher 的 obj 重新链成一个 exe。

── 这一步在整条链路里的位置 ──

    脚本 .py ──编译器──> .bc（含扩展表：只列真的调用到的「模块.函数」）
                            │
                            ├─ 扩展表 → 要链哪几个库、哪几个函数（本文件）
                            │
    launcher 的 obj + aipy.lib + 选中的 pylib ──link.exe──> 新 launcher
                            │
                            再追加 .bc 与尾部索引 ──> 交付 exe

**按需的落点就是这里**，而且是**函数级**：扩展表里没有 `threading.pending`，
`aipy_threading_fn_pending` 就不出现在链接命令行，那个 obj 根本不会被抽出。

── 为什么能按函数挑 ──

静态库的抽取粒度是 **.obj**（`link.exe` 也是）。所以 pylib 侧必须让每个
对外函数独立成一个编译单元（见 libs/threading/src/fn_*.cpp）。打包时用
`/INCLUDE:<符号>` 逐个点名，没点名的 obj 不进产物。

── 两层注册各管什么 ──

"threading" 这个模块名要能被 VM 认出 → /INCLUDE:aipy_pylib_entry_threading
"Thread" 这个函数要能被查到        → /INCLUDE:aipy_threading_fn_Thread

缺第一层，加载器报「扩展函数 threading.Thread 没有链接进来」；缺第二层，
模块在但函数查不到，返回 None。两者都要。

── 为什么不硬编码 link.exe 与库路径 ──

那些路径随 MSVC 版本、随安装位置变。所以从 CMake 的 build 目录读
`CMakeCache.txt`（CMAKE_LINKER / CMAKE_CXX_STANDARD_LIBRARIES）与
`build.ninja`（launcher 的 obj 与库清单）。CMake 已经算过一遍，照抄它
的结果比自己猜可靠。

── 符号命名约定 ──

模块级注册项：  aipy_pylib_entry_<模块>
函数级注册项：  aipy_<模块>_fn_<函数>

符号名前缀由各平台实现（见 tools/aipyic/toolchain.py），这里不硬编码。
命名规则写在 include/aipy/pylib.h 的宏里：`AIPY_PYLIB_MODULE(模块)` 与
`AIPY_PYLIB_FUNCTION(模块, 函数, 实现)`，系统库与用户自定义库共用同一套。
"""

import io
import os
import re
import subprocess

from toolchain import ToolchainError, for_current_host

HERE = os.path.dirname(os.path.abspath(__file__))


class LinkError(Exception):
    """链接失败的原因，直接给命令行用户看。"""


def _read_recipe(library_dir):
    """读 dist/ 里的重链接配方（`link_recipe.json`），没有就返回 None。

    dist/ 布局（与 link.py 平时读的构建目录不同）：

        aipy_app.exe          launcher（纯复制用）
        launcher.obj          重链接时的入口
        link_recipe.json      本文件产出
        lib/aipy.lib          语言运行时
        lib/aipy_base.lib
        lib/<模块>_pylib.lib  按需链接的 pylib

    为什么优先读它：dist 是自包含的发布目录，里面没有 CMakeCache.txt /
    build.ninja。有了配方，拿到 dist 的人不装 CMake 也能打包带扩展的脚本。
    """
    path = os.path.join(library_dir, 'link_recipe.json')
    if not os.path.isfile(path):
        return None
    import json
    with io.open(path, 'r', encoding='utf-8') as handle:
        try:
            recipe = json.load(handle)
        except ValueError:
            return None
    if recipe.get('version') != 1:
        # 不认识的版本按「没有配方」处理：宁可退回读构建目录，也不要拿着
        # 半懂的字段拼出一条错的链接命令。
        return None
    return recipe


def _read_cache(build_dir):
    """读 CMakeCache.txt，取出链接器与标准库。"""
    path = os.path.join(build_dir, 'CMakeCache.txt')
    if not os.path.isfile(path):
        raise LinkError('%s 里没有 CMakeCache.txt，无法确定 link.exe' % build_dir)

    values = {}
    with io.open(path, 'r', encoding='utf-8', errors='replace') as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith('#') or line.startswith('//'):
                continue
            if '=' not in line:
                continue
            key, _, value = line.partition('=')
            if ':' in key:
                key = key.split(':', 1)[0]
            values[key] = value
    return values


def _find_launcher_recipe(build_dir, target_name='aipy_app'):
    """从 build.ninja 里找出 launcher 的 obj 与库清单。

    不解析整份 ninja 语法，只抓我们需要的两行：
      build <target>.exe: ... <obj> | ...
      LINK_LIBRARIES = ...
    """
    path = os.path.join(build_dir, 'build.ninja')
    if not os.path.isfile(path):
        raise LinkError('%s 里没有 build.ninja，请用 Ninja 生成器配置' % build_dir)

    objects = []
    libraries = []
    with io.open(path, 'r', encoding='utf-8', errors='replace') as handle:
        lines = handle.readlines()

    # 目标名在 ninja 里可能带路径前缀；按文件名匹配。
    target_pattern = re.compile(r'^build\s+(?:.*[\\/])?%s\.exe\s*:\s*(\S+)\s+(.*)$'
                                % re.escape(target_name))
    for index, line in enumerate(lines):
        match = target_pattern.match(line.rstrip('\n'))
        if not match:
            continue
        # 第一段是链接规则，后面到 `|` 之前是 obj 与库。
        rest = match.group(2)
        rest = rest.split('|')[0]
        for token in rest.split():
            if token.endswith('.obj'):
                objects.append(token)
            elif token.endswith('.lib'):
                libraries.append(token)
        # 紧接着找 LINK_LIBRARIES（同一 build 语句的变量块）。
        for follow in lines[index + 1:index + 12]:
            stripped = follow.strip()
            if stripped.startswith('LINK_LIBRARIES ='):
                libraries = stripped.split('=', 1)[1].split()
                break
        break

    if not objects:
        raise LinkError('build.ninja 里找不到 %s 的 obj' % target_name)
    if not libraries:
        raise LinkError('build.ninja 里找不到 %s 的 LINK_LIBRARIES' % target_name)
    return objects, libraries


def find_pylib(library_dir, module):
    """按模块名找 `.lib`。约定：`<module>_pylib.lib` 或 `<module>.lib`。

    先看 `library_dir` 本身，再看它的 `lib/` 子目录：dist/ 的布局把运行时
    和 pylib 都放在 `lib/` 下（构建目录则是平铺的），两种都要能找。
    """
    directories = [library_dir, os.path.join(library_dir, 'lib')]
    for directory in directories:
        for candidate in ('%s_pylib.lib' % module, '%s.lib' % module):
            path = os.path.join(directory, candidate)
            if os.path.isfile(path):
                return path
    return None


# 从扩展表取出「模块 → 函数名」的分组。
#
# 扩展表存的是 "threading.Thread" 这样的限定名，已经去重、按首次使用排序。
# 按模块分组是为了决定「查哪个 .lib、点头几个函数符号」。
def group_extensions(qualified_names):
    grouped = []
    for qualified in qualified_names:
        module, _, function = qualified.partition('.')
        if not module or not function:
            continue
        for entry in grouped:
            if entry[0] == module:
                entry[1].append(function)
                break
        else:
            grouped.append((module, [function]))
    return grouped


def _library_symbols(link_exe, library, library_paths):
    """兼容包装：老调用方（测试探针）用这个签名。

    真正的实现在 toolchain.MsvcToolchain.library_names——平台细节收在那里。
    """
    return for_current_host(link_exe, library_paths).library_names(library)


class LinkResult(object):
    """一次按需链接的结果，给调用方打印用。"""

    def __init__(self):
        self.libraries = []      # 用到的 .lib 路径
        self.functions = []      # 精确点名的函数（"模块.函数"）
        self.symbols = []        # 真实传给 /INCLUDE 的符号名
        self.whole_archived = [] # 退回整库链接的 .lib 路径


def relink(build_dir, library_dir, modules, output, extensions=None):
    """按扩展表重链接 launcher，写到 output。返回 LinkResult。

    `modules` 是模块名列表；`extensions` 是完整的「模块.函数」限定名列表。
    后者决定**函数级**裁剪：只 /INCLUDE 真的调用到的函数。

    两种模式：`library_dir` 里有 `link_recipe.json` 时走 dist 模式（不读
    CMake 构建目录、`build_dir` 可以为空）；否则按 CMake 构建目录解析。
    """
    recipe = _read_recipe(library_dir) if library_dir else None
    if recipe is not None:
        # dist 模式：链接器、obj、运行时库都从配方来，路径相对 library_dir。
        link_exe = recipe.get('linker') or ''
        if not os.path.isfile(link_exe):
            raise LinkError('配方里的 link.exe 不存在：%r（换机器后请重新构建 dist）'
                            % link_exe)
        objects = list(recipe.get('objects') or [])
        libraries = list(recipe.get('libraries') or [])
        base_dir = library_dir
        runtime_dir = os.path.join(library_dir, 'lib')
    else:
        cache = _read_cache(build_dir)
        link_exe = cache.get('CMAKE_LINKER')
        if not link_exe or not os.path.isfile(link_exe):
            raise LinkError('CMakeCache.txt 里的 CMAKE_LINKER 不可用：%r' % link_exe)
        objects, libraries = _find_launcher_recipe(build_dir)
        base_dir = build_dir
        runtime_dir = build_dir

    library_paths = _library_paths(link_exe)
    result = LinkResult()

    # 平台层：链接器参数长什么样、库叫什么扩展名、符号怎么命名。
    # 这些是**唯一**与操作系统绑定的部分，全在 toolchain.py 里。
    try:
        toolchain = for_current_host(link_exe, library_paths)
    except ToolchainError as error:
        raise LinkError(str(error))

    # 语言运行时必链；其余按模块名挑。
    always = ('aipy.lib', 'aipy_base.lib')
    used = []
    for module in modules:
        found = find_pylib(library_dir, module)
        if found is None:
            raise LinkError(
                '脚本用到了扩展模块 %s，但 %s 里找不到它的 .lib'
                '（期望 %s_pylib.lib 或 %s.lib）'
                % (module, library_dir, module, module))
        used.append(found)
    result.libraries = used

    # 命令行：配方/CMake 给的库（已是全名）+ 我们选的 pylib。
    command = [link_exe, '/nologo', '/machine:x64', '/subsystem:console',
               '/INCREMENTAL:NO', toolchain.output_flag(output)]

    # 绝对化：link.exe 的工作目录是我们给的，相对路径会找不到。
    for obj in objects:
        full = os.path.join(base_dir, obj)
        if not os.path.isfile(full):
            raise LinkError('找不到 launcher 的 obj：%s' % full)
        command.append(full)

    for lib in libraries:
        if lib in always:
            full = os.path.join(runtime_dir, lib)
            if not os.path.isfile(full):
                raise LinkError('找不到 %s（在 %s 里）' % (lib, runtime_dir))
            # 语言运行时**不能** whole-archive：那会把整个 aipy.lib 拖进来
            # （1.8 MB），产物从 328 KB 涨到几百 KB。它按正常方式链接，
            # 缺什么抽什么；pylib 注册表的哨兵由 bytecode.cpp 引用带进来。
            command.append(full)
            continue
        # 标准库（kernel32 等）原样传，由 LIB 环境变量解析。
        command.append(lib)

    # ── 按需：逐个函数点名 ──
    #
    # 先确认这个库到底提供哪些符号，再决定两种策略：
    #   认得函数级符号 → /INCLUDE 精确点名（最省）
    #   认不得         → /WHOLEARCHIVE 整库进（能跑，但没裁剪）
    #
    # 为什么必须二选一：静态库里**没人引用的 obj 不会被抽出**，而注册项
    # 天生没人引用（它们靠节合并被发现，不是靠符号引用）。所以「什么都不
    # 做」的结局是空产物——探针验证过：不点名时四个函数全部查不到。
    grouped = group_extensions(extensions or [])

    for module, functions in grouped:
        found = find_pylib(library_dir, module)
        if found is None:
            continue
        # 库本身必须出现在命令行上，否则被点名的符号无从解析。
        command.append(found)
        available = toolchain.library_names(found)
        wanted = []
        for function in functions:
            symbol = toolchain.symbol_for(module, function)
            if symbol in available:
                wanted.append(symbol)

        if wanted:
            # 模块级注册项也要点名，否则 VM 认不出 "threading" 这个模块名。
            module_symbol = toolchain.module_symbol(module)
            if module_symbol in available:
                wanted.append(module_symbol)
            for symbol in wanted:
                command.append(toolchain.quote_include(symbol))
                result.symbols.append(symbol)
            for function in functions:
                result.functions.append('%s.%s' % (module, function))
        else:
            # 这个库没提供函数级符号：退回整库链接。
            command.append(toolchain.whole_archive(found))
            result.whole_archived.append(found)

    env = dict(os.environ)
    if library_paths:
        inherited = env.get('LIB')
        env['LIB'] = ';'.join(library_paths) + (';' + inherited if inherited else '')

    completed = subprocess.run(command, capture_output=True, text=True,
                               encoding='utf-8', errors='replace', env=env)
    if completed.returncode != 0:
        raise LinkError('link.exe 失败：\n%s\n%s'
                        % (completed.stdout.strip(), completed.stderr.strip()))
    return result


def _library_paths(link_exe):
    """从 link.exe 的位置推断 MSVC 与 Windows SDK 的库目录。

    Windows SDK 的路径无法从 link.exe 推出来（独立安装），所以扫常见根目录。
    MSVC 库目录则逐级上溯找 `lib/x64/libcpmt.lib`——数 `..` 的层数不可靠，
    不同版本目录深度会变，数错一级就静默丢掉整个 MSVC 库路径。
    """
    paths = []
    probe = os.path.dirname(os.path.abspath(link_exe))
    for _ in range(8):
        candidate = os.path.join(probe, 'lib', 'x64')
        if os.path.isfile(os.path.join(candidate, 'libcpmt.lib')):
            paths.append(candidate)
            break
        parent = os.path.dirname(probe)
        if parent == probe:
            break
        probe = parent

    roots = [
        r'C:\Program Files (x86)\Windows Kits\10\Lib',
        r'C:\Program Files\Windows Kits\10\Lib',
    ]
    for root in roots:
        if not os.path.isdir(root):
            continue
        versions = sorted((name for name in os.listdir(root)
                           if os.path.isdir(os.path.join(root, name))),
                          reverse=True)
        if not versions:
            continue
        base = os.path.join(root, versions[0])
        for part in ('ucrt\\x64', 'um\\x64'):
            candidate = os.path.join(base, part)
            if os.path.isdir(candidate):
                paths.append(candidate)
    return paths
