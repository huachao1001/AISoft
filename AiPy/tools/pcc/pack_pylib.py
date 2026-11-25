"""构建期打 .pylib：ZIP（deflate）里放 manifest + 静态库 + 可选字节码。

为什么写包用 Python 而读包用 C++：
  - 写包只在构建期发生，那时一定有 Python，`zipfile` 直接可用
  - 读包发生在用户机器上（打包脚本时），那里不能有 Python

这和「编译器必须是 C++」是同一条约束的两个方向。

函数清单从静态库自己读，不依赖 link.exe 的文本输出：
  MSVC 当前的 link.exe 已不认 /dump，而且文本解析会随版本变。
  库是 ar 归档，符号表是确定格式。
"""

import argparse
import io
import json
import os
import struct
import sys
import zipfile

MANIFEST_FORMAT_VERSION = 1

# pylib 注册宏展开出的符号前缀/后缀，与 include/aipy/pylib.h 一致。
FUNCTION_PREFIX = 'aipy_%s_fn_'
MODULE_PREFIX = 'aipy_pylib_entry_'

# 归档里原生库的平台目录。一个包可以同时带多份，布局本身就能回答
# 「这个库支持哪些平台」——不解包、不读额外文件，看目录名即可。
DEFAULT_PLATFORM = 'win-x64'


def read_archive_symbols(path):
    """读 COFF 静态库里所有外部定义符号（见 tools/pcc 的 coff.cpp）。

    两层：先读 ar 索引（只含被引用符号），再逐成员解析 COFF 符号表
    （覆盖注册项这种没人引用的符号）。
    """
    data = open(path, 'rb').read()
    if data[:8] != b'!<arch>\n':
        raise SystemExit('%s is not an ar archive' % path)

    members = []          # [(name, body_offset, size)]
    index_member = None   # (body_offset, size)
    longnames = b''

    position = 8
    while position + 60 <= len(data):
        header = data[position:position + 60]
        raw = header[0:16].decode('ascii', 'replace').rstrip()
        try:
            size = int(header[48:58].decode('ascii').strip() or 0)
        except ValueError:
            break
        body = position + 60

        if raw == '//':
            longnames = data[body:body + size]
        elif raw == '/':
            index_member = (body, size)
        elif raw.startswith('/') and raw[1:].strip().isdigit():
            offset = int(raw[1:].strip())
            stop = longnames.find(b'\x00', offset)
            if stop < 0:
                stop = longnames.find(b'/\n', offset)
            if stop < 0:
                stop = len(longnames)
            name = longnames[offset:stop].decode('ascii', 'replace')
            members.append((os.path.basename(name), body, size))
        elif raw:
            members.append((os.path.basename(raw.rstrip('/')), body, size))

        position = body + size
        if position % 2:
            position += 1

    symbols = set()

    # 第一层：ar 索引（大端）
    if index_member is not None:
        at, size = index_member
        count = struct.unpack_from('>I', data, at)[0]
        cursor = at + 4 + count * 4
        for _ in range(count):
            stop = data.find(b'\x00', cursor, at + size)
            if stop < 0:
                break
            name = data[cursor:stop].decode('ascii', 'replace')
            if name:
                symbols.add(name)
            cursor = stop + 1

    # 第二层：逐成员 COFF 符号表
    for name, body, size in members:
        if not name.endswith('.obj'):
            continue
        blob = data[body:body + size]
        if len(blob) < 20:
            continue
        symbol_at, symbol_count = struct.unpack_from('<II', blob, 8)
        if not symbol_at or not symbol_count:
            continue
        strings_at = symbol_at + symbol_count * 18
        strings = b''
        if strings_at + 4 <= len(blob):
            length = struct.unpack_from('<I', blob, strings_at)[0]
            if 4 <= length <= len(blob) - strings_at:
                strings = blob[strings_at:strings_at + length]

        cursor = 0
        while cursor < symbol_count:
            entry = symbol_at + cursor * 18
            if entry + 18 > len(blob):
                break
            raw_name = blob[entry:entry + 8]
            section, = struct.unpack_from('<h', blob, entry + 12)
            storage = blob[entry + 16]
            aux = blob[entry + 17]

            if raw_name[:4] == b'\x00\x00\x00\x00':
                offset = struct.unpack_from('<I', raw_name, 4)[0]
                if offset < len(strings):
                    stop = strings.find(b'\x00', offset)
                    if stop < 0:
                        stop = len(strings)
                    text = strings[offset:stop].decode('ascii', 'replace')
                else:
                    text = ''
            else:
                text = raw_name.rstrip(b'\x00').decode('ascii', 'replace')

            if storage == 2 and section > 0 and text:
                symbols.add(text)
            cursor += 1 + aux

    return symbols


def module_functions(path, module, symbol_prefix=None):
    """从符号集里挑出本模块注册的函数名。

    symbol_prefix 是符号里的模块前缀。点分模块名（`os.path`）在 C++ 标识符
    里不能有点，所以符号用下划线版本（`aipy_os_path_fn_exists`）；而 manifest
    的 module 字段与文件名仍用带点的真名。不给这个参数时就是模块名本身。
    """
    prefix_module = symbol_prefix or module
    fn_prefix = FUNCTION_PREFIX % prefix_module
    names = []
    for symbol in read_archive_symbols(path):
        if symbol.startswith(fn_prefix):
            names.append(symbol[len(fn_prefix):])
    return sorted(set(names))


def module_bare_names(path, module, symbol_prefix=None):
    """从符号集里挑出本模块注册的**裸名**（AIPY_PYLIB_BUILTIN）。

    裸名让脚本像用内建那样直接调用（`open(p)` 而不是 `fs.open(p)`）。打包
    时必须把它写进 manifest：链接器靠它才知道「这个脚本用了 open，得去找
    声明了 open 的库」。

    符号形如 `aipy_fs_bare_open`，取最后一段即裸名本身。
    """
    prefix_module = symbol_prefix or module
    bare_prefix = 'aipy_%s_bare_' % prefix_module
    names = []
    for symbol in read_archive_symbols(path):
        if symbol.startswith(bare_prefix):
            names.append(symbol[len(bare_prefix):])
    return sorted(set(names))


def main(argv):
    parser = argparse.ArgumentParser(description='pack a .pylib archive')
    parser.add_argument('--module', required=True, help='module name, e.g. threading')
    parser.add_argument('--library', required=True, action='append',
                        help='native static library; repeatable for archives '
                             'that carry several files (core.pylib)')
    parser.add_argument('--library-role', action='append', default=[],
                        help='role name for the matching --library, in order. '
                             'core.pylib uses roles like "runtime", "base", '
                             '"launcher"; the linker reads them by role')
    parser.add_argument('--output', required=True, help='output .pylib path')
    parser.add_argument('--bytecode', help='optional Python layer bytecode')
    parser.add_argument('--version', default='1', help='module version string')
    parser.add_argument('--platform', default=DEFAULT_PLATFORM,
                        help='platform directory for the native library '
                             '(default: %s)' % DEFAULT_PLATFORM)
    parser.add_argument('--symbol-prefix',
                        help='module prefix used in symbol names; defaults to '
                             '--module. Needed for dotted modules like os.path, '
                             'whose symbols use os_path')
    parser.add_argument('--no-functions', action='store_true',
                        help='skip the symbol scan. core.pylib has no script '
                             'functions, so scanning it would only cost time')
    options = parser.parse_args(argv)

    libraries = options.library
    roles = list(options.library_role)
    if roles and len(roles) != len(libraries):
        raise SystemExit('--library-role must be given once per --library')
    if not roles:
        roles = [options.module] * len(libraries)

    functions = [] if options.no_functions else module_functions(
        libraries[0], options.module, options.symbol_prefix)
    bare_names = [] if options.no_functions else module_bare_names(
        libraries[0], options.module, options.symbol_prefix)

    # 原生库按平台分目录：同一个包可以带多份实现，而「支持哪些平台」看
    # 目录名或 platforms 表就知道，不需要额外的探测。
    #
    # 扩展名也按平台走：链接器是靠扩展名判断「这是不是一个库」的，Windows
    # 上叫 `.a` 的文件它根本不看。
    suffix = '.a' if options.platform.startswith('linux') or \
        options.platform.startswith('darwin') else '.lib'

    # 一个平台可以有多个文件（core.pylib 就是这种）：它们必须一起出现、
    # 一起解出来，所以写进同一个平台条目下的列表，而不是散成几个平台键。
    #
    # 单个文件的库仍然用 platforms 那张「平台 → 单一路径」表——那是绝大多数
    # 模块的形态，不该为了 core 的特殊情况把普遍形态也改写一遍。
    if len(libraries) == 1:
        native_path = 'lib/%s/%s%s' % (options.platform, options.module, suffix)
        platforms = {options.platform: native_path}
        files = [(native_path, libraries[0])]
    else:
        platforms = {}
        files = []
        # 文件名用角色名，链接器按角色找（runtime / base / launcher）。
        # 不用原始文件名：`aipy.lib` 这种名字在归档里没有意义，而
        # 「哪个是运行时、哪个是入口」才是读取端真正要的信息。
        for role, path in zip(roles, libraries):
            files.append(('lib/%s/%s%s' % (options.platform, role, suffix),
                          path))

    manifest = {
        'module': options.module,
        'formatVersion': MANIFEST_FORMAT_VERSION,
        'version': options.version,
        # 归档内路径一律用 /，与 ZIP 规范一致。
        'platforms': platforms,
        'functions': functions,
        # 裸名：脚本里可以直接写的短名（`open` → `fs.open`）。空的时候
        # 不写这个字段，好让旧读取端不受影响。
        'bare_names': bare_names,
    }
    if len(libraries) > 1:
        # 平台 → 角色 → 归档内路径。读取端按角色取，不按文件名猜。
        manifest['platform_files'] = {
            options.platform: {role: path for role, (path, _) in zip(roles, files)}
        }
    if options.bytecode:
        manifest['bytecode'] = 'bytecode/%s.bc' % options.module

    os.makedirs(os.path.dirname(os.path.abspath(options.output)), exist_ok=True)
    with zipfile.ZipFile(options.output, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('manifest.json',
                         json.dumps(manifest, indent=2, sort_keys=True) + '\n')
        for stored_path, source in files:
            archive.write(source, stored_path)
        if options.bytecode:
            archive.write(options.bytecode,
                          'bytecode/%s.bc' % options.module)

    if options.no_functions:
        print('%s -> %s [%s] (%d files: %s)'
              % (options.module, options.output, options.platform, len(files),
                 ', '.join(roles)))
        return 0

    extra = ''
    if bare_names:
        extra = ', bare: %s' % ', '.join(bare_names)
    print('%s -> %s [%s] (%d functions: %s%s)'
          % (options.module, options.output, options.platform, len(functions),
             ', '.join(functions) if functions else 'none', extra))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
