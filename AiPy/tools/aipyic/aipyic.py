#!/usr/bin/env python3
"""AiPy 命令行编译入口：把脚本编成字节码、反汇编、以及打包成 exe。

用法：

    python tools/aipyic/aipyic.py app.py            # 生成 app.bc
    python tools/aipyic/aipyic.py app.py out.bc     # 指定输出
    python tools/aipyic/aipyic.py --disasm app.bc   # 反汇编看指令流
    python tools/aipyic/aipyic.py --exe app.py app.exe            # 打包成 exe
    python tools/aipyic/aipyic.py --exe app.py app.exe --launcher aipy_app.exe
    python tools/aipyic/aipyic.py --unbundle app.exe app.bc       # 取回字节码

── 为什么反汇编要写在 Python 这边 ──

调试编译器时最常问的是「这一行源代码生成了哪几条指令」。C++ 侧
`vm::disassemble` 也能打，但它要先编一遍、再传文件，改一次编译器就得
重建。Python 侧直接读 `.bc`、直接打，反馈是亚秒级的。

两份反汇编器（Python 和 C++）也不算重复：它们读同一个文件、各自解码，
**对不上就说明加载器或格式有 bug**——这是一份免费的一致性证据。

── 打包为什么不编译 C++ ──

`--exe` 是把字节码追加到预编译 launcher 的尾部（格式见 bundle.py），
不调用任何编译器。用户机器上不需要 MSVC，甚至不需要链接器。
"""

import io
import os
import struct
import sys

# 不写 .pyc：这套脚本是**发布件**（dist/aipyic/），跑一次就在旁边留下
# `__pycache__`，而那个目录会被跟着发布出去——里面是构建机的 Python 版本
# 编的字节码，到了别人机器上匹配不上，纯属垃圾。关掉这一次写盘的收益远大于
# 省下的几毫秒（编译器本身要跑几十毫秒）。
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import bundle
import compiler
import link
import pack

# 指令 → 操作数说明。空字符串表示无操作数。
# 这份表与 compiler.py 的 emit 调用一一对应，漏写一条反汇编就会错位，
# 所以 decode 时对未知操作数长度直接报错而不是猜。
OPERANDS = {
    'CONST': ('u16',),
    'LOAD_LOCAL': ('u16',),
    'STORE_LOCAL': ('u16',),
    'LOAD_GLOBAL': ('u16',),
    'STORE_GLOBAL': ('u16',),
    'BIN': ('u8',),
    'CMP': ('u8',),
    'UNARY': ('u8',),
    'JUMP': ('u16',),
    'JUMP_IF_FALSE': ('u16',),
    'JUMP_IF_FALSE_KEEP': ('u16',),
    'JUMP_IF_TRUE_KEEP': ('u16',),
    'CALL': ('u16', 'u8'),
    'CALL_BUILTIN': ('u16', 'u8'),
    'CALL_METHOD': ('u16', 'u8'),
    'CALL_EXT': ('u16', 'u8'),
    'BUILD_LIST': ('u16',),
    'BUILD_DICT': ('u16',),    'GET_ITEM': (),
    'SET_ITEM': (),
    'SLICE': ('u8',),
    'ITER_INIT': (),
    'ITER_NEXT': ('u16',),
    'ITER_BREAK': (),
    'INPLACE': ('u8',),
    # u16 局部槽号（存 with 的管理器）+ u16「异常被吞掉时的跳转目标」。
    'WITH_ENTER': ('u16', 'u16'),
    'WITH_EXIT': ('u16',),
    'NONE': (),
    'TRUE': (),
    'FALSE': (),
    'POP': (),
    'DUP': (),
    'RETURN': (),
    # 末尾追加的指令
    'CALL_DYN': ('u16',),
    'DICT_MERGE': (),
    'MAKE_FUNCTION': ('u16', 'u16'),
    'CALL_VALUE': ('u8',),
    'CALL_VALUE_DYN': (),
    'MAKE_CLASS': ('u16', 'u16', 'u16'),
    'GET_ATTR': ('u16',),
    'SET_ATTR': ('u16',),
    'INSTANTIATE': ('u8',),
    'MAKE_SUPER': ('u16',),
    'YIELD': (),
    'AWAIT': (),
    'LOAD_TYPE': ('u16',),
    'UNPACK': ('u16',),
    'CALL_BUILTIN_KW': ('u16', 'u8', 'u8'),
    'MAKE_BUILTIN_FUNCTION': ('u16',),
    'CALL_METHOD_KW': ('u16', 'u8', 'u8'),
    'BUILD_TUPLE': ('u16',),
    'BUILD_SET': ('u16',),
    'DEL_LOCAL': ('u16',),
    'DEL_GLOBAL': ('u16',),
    'SET_SLICE': ('u8',),
    'SETUP_EXC': ('u16',),
    'POP_EXC': (),
    'RAISE': (),
    'RAISE_TYPE': ('u16', 'u8'),
    'RERAISE': (),
    'EXC_MATCH': ('u16', 'u16'),
    'CURRENT_EXC': (),
    'CLEAR_EXC': (),
}

BINOP_NAMES = compiler.BINOPS
CMPOP_NAMES = compiler.CMPOPS
UNOP_NAMES = compiler.UNOPS


class BinaryReader:
    """按 pack.py 的写出顺序读回来。格式权威定义在 pack.py 头部。"""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, count):
        if self.pos + count > len(self.data):
            raise SystemExit('字节码在偏移 %d 处提前结束' % self.pos)
        chunk = self.data[self.pos:self.pos + count]
        self.pos += count
        return chunk

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack('<H', self.take(2))[0]

    def u32(self):
        return struct.unpack('<I', self.take(4))[0]

    def i32(self):
        return struct.unpack('<i', self.take(4))[0]

    def i64(self):
        return struct.unpack('<q', self.take(8))[0]

    def f64(self):
        return struct.unpack('<d', self.take(8))[0]

    def text(self):
        length = self.u32()
        return self.take(length).decode('utf-8')


def decode(code, consts, extensions=None):
    """把一段字节码展开成 [(偏移, 名字, 操作数描述)]。"""
    out = []
    pos = 0
    while pos < len(code):
        offset = pos
        index = code[pos]
        pos += 1
        if index >= len(compiler.OPCODES):
            raise SystemExit('偏移 %d 处的操作码 %d 越界' % (offset, index))
        name = compiler.OPCODES[index]
        shapes = OPERANDS.get(name)
        if shapes is None:
            raise SystemExit('反汇编表缺少指令 %s' % name)

        operands = []
        for shape in shapes:
            if shape == 'u8':
                operands.append(code[pos])
                pos += 1
            elif shape == 'u16':
                operands.append(struct.unpack('<H', code[pos:pos + 2])[0])
                pos += 2
            else:
                raise SystemExit('未知操作数类型 %s' % shape)

        # 把裸数字翻译成人能读的东西。
        if name == 'CONST' and operands[0] < len(consts):
            text = repr(consts[operands[0]])
        elif name == 'BIN' and operands[0] < len(BINOP_NAMES):
            text = BINOP_NAMES[operands[0]]
        elif name == 'INPLACE' and operands[0] < len(BINOP_NAMES):
            text = BINOP_NAMES[operands[0]]
        elif name == 'CMP' and operands[0] < len(CMPOP_NAMES):
            text = CMPOP_NAMES[operands[0]]
        elif name == 'UNARY' and operands[0] < len(UNOP_NAMES):
            text = UNOP_NAMES[operands[0]]
        elif name == 'CALL_METHOD' and operands[0] < len(consts):
            text = '%r, argc=%d' % (consts[operands[0]], operands[1])
        elif name == 'CALL_EXT':
            # 扩展号指向扩展表，不是常量池——两者都用 u16，很容易看错。
            if extensions is not None and operands[0] < len(extensions):
                text = '%s, argc=%d' % (extensions[operands[0]], operands[1])
            else:
                text = 'ext#%d, argc=%d' % (operands[0], operands[1])
        elif name == 'CALL_BUILTIN':
            text = 'builtin#%d, argc=%d' % (operands[0], operands[1])
        elif name in ('JUMP', 'JUMP_IF_FALSE', 'JUMP_IF_FALSE_KEEP',
                      'JUMP_IF_TRUE_KEEP', 'ITER_NEXT'):
            text = '-> %d' % operands[0]
        elif name == 'SLICE':
            flags = operands[0]
            parts = [label for bit, label in ((1, 'start'), (2, 'stop'), (4, 'step'))
                     if flags & bit]
            text = '有 ' + '/'.join(parts) if parts else '全默认'
        else:
            text = ', '.join(str(value) for value in operands)
        out.append((offset, name, text))
    return out


def load(path):
    with io.open(path, 'rb') as handle:
        data = handle.read()
    if len(data) < 10 or data[:8] != pack.MAGIC:
        raise SystemExit('%s 不是 AiPy 字节码文件' % path)
    version = struct.unpack('<H', data[8:10])[0]
    if version != pack.VERSION:
        raise SystemExit('字节码版本 %d 与当前 %d 不一致' % (version, pack.VERSION))

    reader = BinaryReader(data[10:])
    program = {}
    program['globalCount'] = reader.u16()
    program['localCount'] = reader.u16()
    const_count = reader.u16()
    function_count = reader.u16()
    builtin_count = reader.u16()
    import_count = reader.u16()
    extension_count = reader.u16()

    program['globalNames'] = [reader.text() for _ in range(reader.u16())]

    consts = []
    for _ in range(const_count):
        kind = reader.u8()
        if kind == pack.CONST_NONE:
            consts.append(None)
        elif kind == pack.CONST_BOOL:
            consts.append(reader.i64() != 0)
        elif kind == pack.CONST_INT:
            consts.append(reader.i64())
        elif kind == pack.CONST_FLOAT:
            consts.append(reader.f64())
        elif kind == pack.CONST_STR:
            consts.append(reader.text())
        else:
            raise SystemExit('未知常量类型 %d' % kind)
    program['consts'] = consts

    functions = []
    for _ in range(function_count):
        function = {}
        function['paramCount'] = reader.u16()
        function['localCount'] = reader.u16()
        code_len = reader.u32()
        line_count = reader.u16()
        name_len = reader.u16()
        function['name'] = reader.take(name_len).decode('utf-8')
        function['code'] = reader.take(code_len)
        function['lines'] = [(reader.u32(), reader.i32()) for _ in range(line_count)]
        functions.append(function)
    program['functions'] = functions

    code_len = reader.u32()
    line_count = reader.u16()
    program['code'] = reader.take(code_len)
    program['lines'] = [(reader.u32(), reader.i32()) for _ in range(line_count)]

    program['builtins'] = [reader.text() for _ in range(builtin_count)]
    program['imports'] = [reader.text() for _ in range(import_count)]
    program['extensions'] = [reader.text() for _ in range(extension_count)]
    return program


def line_of(lines, offset):
    line = 0
    for entry_offset, entry_line in lines:
        if entry_offset > offset:
            break
        line = entry_line
    return line


def disassemble(path):
    program = load(path)
    print('%s：%d 条全局槽，%d 个常量，%d 个函数'
          % (path, program['globalCount'], len(program['consts']),
             len(program['functions'])))
    if program['builtins']:
        print('内建：%s' % ', '.join(program['builtins']))

    blocks = [('<module>', program['code'], program['lines'], 0)]
    for function in program['functions']:
        blocks.append((function['name'], function['code'], function['lines'],
                       function['paramCount']))

    for name, code, lines, params in blocks:
        header = '── %s' % name
        if name != '<module>':
            header += '（%d 个参数）' % params
        print('')
        print(header)
        for offset, opname, text in decode(code, program['consts'],
                                           program.get('extensions')):
            where = line_of(lines, offset)
            print('  %4d  L%-4d %-20s %s' % (offset, where, opname, text))
    return 0


def find_launcher(explicit):
    """定位 launcher。给了路径就用；没给就在常见构建目录里找。"""
    if explicit:
        if not os.path.isfile(explicit):
            raise bundle.BundleError('找不到 launcher：%s' % explicit)
        return explicit

    root = os.path.normpath(os.path.join(HERE, '..', '..'))
    # 构建目录不止一个（build / build/final / build/clean …），按修改时间
    # 挑最新的那个：换了构建目录还在用旧 launcher，症状是「新打包的 exe
    # 不认识新指令」。找不到时给出明确的编译命令，而不是猜。
    candidates = []
    for base in ('build', '.'):
        folder = os.path.join(root, base)
        if not os.path.isdir(folder):
            continue
        for dirpath, _, filenames in os.walk(folder):
            if 'aipy_app.exe' in filenames:
                candidates.append(os.path.join(dirpath, 'aipy_app.exe'))
    if not candidates:
        raise bundle.BundleError(
            '找不到 aipy_app.exe。先编译一次：\n'
            '  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release\n'
            '  cmake --build build\n'
            '或者用 --launcher 指定路径')
    candidates.sort(key=os.path.getmtime, reverse=True)
    return candidates[0]


def extension_modules(program):
    """从扩展表里取出模块名，去重、保持首次出现顺序。

    按需链接的判据是**扩展表**（真的调用了）而不是 import 表（只是提到）：
    `import text` 但一个 text 函数都没用，就不该把 text.pylib 链进去。
    """
    modules = []
    for qualified in program.get('extensions') or []:
        module = qualified.split('.', 1)[0]
        if module and module not in modules:
            modules.append(module)
    return modules


def bundle_main(source, output, launcher_path, build_dir, library_dir):
    """脚本 → 字节码 →（按需重链接）→ 追加到 launcher → 独立 exe。"""
    if not os.path.isfile(source):
        print('找不到 %s' % source, file=sys.stderr)
        return 2
    if not output:
        print('用法: aipyic.py --exe <script.py> <app.exe>', file=sys.stderr)
        return 2

    try:
        program = compiler.compile_file(source)
    except compiler.CompileError as error:
        print(str(error), file=sys.stderr)
        return 1

    modules = extension_modules(program)

    # 中间字节码写到临时目录：它是派生物，不该在源码树里留下 .bc。
    import tempfile
    with tempfile.TemporaryDirectory() as scratch:
        bytecode = os.path.join(scratch, 'app.bc')
        pack.write_file(program, bytecode)

        launcher = find_launcher(launcher_path)
        link_result = None
        # 用到扩展时才重新链接；否则沿用预编译 launcher（纯文件复制，最快）。
        if modules:
            # dist 里带 link_recipe.json，拿到它的目录就能重链接，不需要
            # --build-dir；否则必须给构建目录（要从里面读 CMake 配方）。
            dist_mode = bool(library_dir) and os.path.isfile(
                os.path.join(library_dir, 'link_recipe.json'))
            if not build_dir and not dist_mode:
                print('脚本用到了扩展模块 %s，需要 --build-dir（或带 '
                      'link_recipe.json 的 --lib-dir）才能重链接'
                      % ', '.join(modules), file=sys.stderr)
                return 2
            if not library_dir:
                library_dir = os.path.dirname(build_dir)
            try:
                relinked = os.path.join(scratch, 'launcher.exe')
                link_result = link.relink(build_dir, library_dir, modules, relinked,
                                          extensions=program.get('extensions'))
                launcher = relinked
            except link.LinkError as error:
                print(str(error), file=sys.stderr)
                return 1

        try:
            size = bundle.bundle(launcher, bytecode, output)
        except bundle.BundleError as error:
            print(str(error), file=sys.stderr)
            return 1
        payload = os.path.getsize(bytecode)

    summary = '%s -> %s（%d 字节 = launcher %d KB + 脚本 %d 字节）' \
              % (source, output, size, (size - payload) // 1024, payload)
    if link_result is not None:
        if link_result.functions:
            # 函数级按需的可见证据：列出实际点名了哪些函数。
            summary += '\n  链接的函数：%s' % ', '.join(link_result.functions)
        if link_result.whole_archived:
            summary += '\n  整库链接（未提供函数级符号）：%s' % ', '.join(
                os.path.basename(p) for p in link_result.whole_archived)
    print(summary)
    return 0


def main(argv):
    args = list(argv[1:])
    if not args:
        print(__doc__, file=sys.stderr)
        return 2

    if args[0] in ('-d', '--disasm'):
        if len(args) < 2:
            print('用法: aipyic.py --disasm <file.bc>', file=sys.stderr)
            return 2
        return disassemble(args[1])

    if args[0] == '--exe':
        # 这三个可以出现在任意位置，先摘出来。
        rest = args[1:]
        launcher_path = None
        build_dir = None
        library_dir = None
        while True:
            moved = False
            for flag, slot in (('--launcher', 'launcher'),
                               ('--build-dir', 'build'),
                               ('--lib-dir', 'lib')):
                if flag in rest:
                    at = rest.index(flag)
                    if at + 1 >= len(rest):
                        print('%s 缺少路径' % flag, file=sys.stderr)
                        return 2
                    value = rest[at + 1]
                    del rest[at:at + 2]
                    if slot == 'launcher':
                        launcher_path = value
                    elif slot == 'build':
                        build_dir = value
                    else:
                        library_dir = value
                    moved = True
                    break
            if not moved:
                break
        if len(rest) < 2:
            print('用法: aipyic.py --exe <script.py> <app.exe> '
                  '[--launcher <aipy_app.exe>] [--build-dir <cmake 构建目录>] '
                  '[--lib-dir <pylib 目录>]', file=sys.stderr)
            return 2
        return bundle_main(rest[0], rest[1], launcher_path, build_dir, library_dir)

    if args[0] == '--unbundle':
        if len(args) < 3:
            print('用法: aipyic.py --unbundle <app.exe> <out.bc>', file=sys.stderr)
            return 2
        try:
            size = bundle.extract(args[1], args[2])
        except bundle.BundleError as error:
            print(str(error), file=sys.stderr)
            return 1
        print('%s -> %s（%d 字节）' % (args[1], args[2], size))
        return 0

    source = args[0]
    if not os.path.isfile(source):
        print('找不到 %s' % source, file=sys.stderr)
        return 2
    output = args[1] if len(args) > 1 else os.path.splitext(source)[0] + '.bc'

    try:
        program = compiler.compile_file(source)
    except compiler.CompileError as error:
        print(str(error), file=sys.stderr)
        return 1

    size = pack.write_file(program, output)
    print('%s -> %s（%d 字节，%d 个函数，%d 条模块指令）'
          % (source, output, size, len(program['functions']), len(program['code'])))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
