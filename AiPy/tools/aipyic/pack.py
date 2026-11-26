#!/usr/bin/env python3
"""把字节码编译成 AiPy 能直接读的二进制格式。

── 为什么不是 JSON ──

JSON 要一个解析器。`nlohmann/json` 单头约 1 MB 源码，编进产物就是几百 KB；
自己写一个正确的 JSON 解析器要上千行，还要处理转义、Unicode、深度限制。
而这份数据只在**构建期**生成、**启动时**读一次，格式由我们自己定，没有任何
互操作需求。

── 格式（小端序，全部定长）──

  magic          "AIPYBC01"            8 字节
  version        u16                   格式版本
  globalCount    u16
  localCount     u16
  constCount     u16
  functionCount  u16
  builtinCount   u16                   只用前 N 个（保持顺序稳定）
  importCount    u16
  extensionCount u16

  globalNameCount u16
  globalNames[]  globalNameCount × (u32 长度 + UTF-8 字节，按槽号排列)

  consts[]       每个：
    type         u8    0=none 1=bool 2=int 3=float 4=str
    载荷：
      bool/int   i64
      float      f64
      str        u32 长度 + UTF-8 字节

  functions[]    每个：
    paramCount   u16
    localCount   u16
    codeLen      u32
    lineLen      u16
    nameLen      u16
    name         nameLen 字节
    code         codeLen 字节
    lines[]      lineLen × (u32 offset + i32 line)

  module:
    codeLen      u32
    lineLen      u16
    code         codeLen 字节
    lines[]      lineLen × (u32 offset + i32 line)

  builtins[]     builtinCount × (u32 长度 + UTF-8 字节)
                 名字按 CALL_BUILTIN 下标排列。加载器按名字查函数指针，
                 查不到就报错——静默跳过会让脚本调到别的函数。
  imports[]      importCount × (u32 长度 + UTF-8 字节)
  extensions[]   extensionCount × (u32 长度 + UTF-8 字节)
                 「模块.函数」限定名，按 CALL_EXT 下标排列。加载器拿它去
                 已链接的 pylib 里查函数指针；查不到不算错（按需链接的
                 代价），调用到那一行时才报「没有链接进来」。

── 全局名字为什么进产物 ──

热加载要把旧状态灌进新程序，而新程序里同一个变量的槽号可能变了（加一个
变量就整体移位）。只有按名字匹配才能保住状态；按槽号会把 count 的值灌进
label。测试也按名字取值，所以槽号调整不会让断言悄悄指错变量。

── 为什么不复用 JSON 那份中间表示 ──

JSON 是给人看的（调试时 `compiler.py script.py` 直接打印）。二进制是给
加载器的。两者由同一份编译结果生成，不是两套逻辑：`compiler.py` 产出
dict，`pack()` 把它写成二进制。所以不会有「JSON 对、二进制错」这种漂移。
"""

import io
import struct

MAGIC = b'AIPYBC01'
# 2：加入扩展表（extensions）。旧版产物没有它，加载器会读到位错的数据，
# 所以必须升版本、让旧 .bc 在加载时报「版本不一致」而不是算出乱码。
VERSION = 2

# 常量类型标记，与 cpp 侧 ConstType 一致。
CONST_NONE = 0
CONST_BOOL = 1
CONST_INT = 2
CONST_FLOAT = 3
CONST_STR = 4
CONST_BYTES = 5


class Writer:
    def __init__(self):
        self.parts = []

    def u8(self, value):
        self.parts.append(struct.pack('<B', value & 0xFF))

    def u16(self, value):
        self.parts.append(struct.pack('<H', value & 0xFFFF))

    def u32(self, value):
        self.parts.append(struct.pack('<I', value & 0xFFFFFFFF))

    def i32(self, value):
        self.parts.append(struct.pack('<i', value))

    def i64(self, value):
        self.parts.append(struct.pack('<q', value))

    def f64(self, value):
        self.parts.append(struct.pack('<d', value))

    def raw(self, data):
        self.parts.append(data)

    def text(self, value):
        # bytes 常量可能含任意字节（\xff 这类），不能走 UTF-8 编码。
        data = value if isinstance(value, bytes) else value.encode('utf-8')
        self.u32(len(data))
        self.raw(data)

    def bytes(self):
        return b''.join(self.parts)


def pack(program):
    """把 compiler.py 产出的 dict 写成二进制。"""
    # 顺序必须与 include/aipy/bytecode.h 的读取顺序一致。
    out = Writer()
    out.raw(MAGIC)
    out.u16(VERSION)

    out.u16(program['globalCount'])
    out.u16(program['localCount'])
    out.u16(len(program['consts']))
    out.u16(len(program['functions']))

    builtins = program.get('builtins') or []
    out.u16(len(builtins))
    imports = program.get('imports') or []
    out.u16(len(imports))
    extensions = program.get('extensions') or []
    out.u16(len(extensions))

    # 全局变量名，按槽号排列。
    #
    # 为什么名字必须进产物：热加载要「把旧状态灌进新程序」，而新程序里
    # 同一个变量的槽号可能变了（加了一个变量就整体移位）。只有按名字匹配
    # 才能保住状态；按槽号会把 `count` 的值灌进 `label`。
    globals_ = program.get('globals') or []
    out.u16(len(globals_))
    for name in globals_:
        out.text(name)

    for const in program['consts']:
        kind = const['type']
        if kind == 'bool':
            out.u8(CONST_BOOL)
            out.i64(1 if const['value'] else 0)
        elif kind == 'int':
            out.u8(CONST_INT)
            out.i64(const['value'])
        elif kind == 'float':
            out.u8(CONST_FLOAT)
            out.f64(const['value'])
        elif kind == 'str':
            out.u8(CONST_STR)
            out.text(const['value'])
        elif kind == 'bytes':
            out.u8(CONST_BYTES)
            out.text(const['value'])
        else:
            raise ValueError('未知常量类型 %r' % (kind,))

    for function in program['functions']:
        out.u16(function['paramCount'])
        out.u16(function['localCount'])
        out.u32(len(function['code']))
        out.u16(len(function['lines']))
        name = function.get('name', '?').encode('utf-8')
        out.u16(len(name))
        out.raw(name)
        out.raw(bytes(function['code']))
        for offset, line in function['lines']:
            out.u32(offset)
            out.i32(line)

    module = program
    out.u32(len(module['code']))
    out.u16(len(module['lines']))
    out.raw(bytes(module['code']))
    for offset, line in module['lines']:
        out.u32(offset)
        out.i32(line)

    for name in builtins:
        out.text(name)
    for name in imports:
        out.text(name)
    for name in extensions:
        out.text(name)

    return out.bytes()


def write_file(program, path):
    data = pack(program)
    with io.open(path, 'wb') as handle:
        handle.write(data)
    return len(data)


if __name__ == '__main__':
    import sys
    sys.exit('这是库，请用 compiler.py')
