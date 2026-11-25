#!/usr/bin/env python3
"""把字节码接到 launcher 的尾部，产出一个可直接运行的 exe。

── 为什么是「追加」而不是资源段 ──

三种做法都能把脚本塞进 exe：

1. **资源段（RT_RCDATA）**：要 `rc.exe` 编译 .rc，再用 `UpdateResource` 或
   链接器合并。打包链路因此需要额外工具，而且改资源要重写 PE 节表。
2. **C++ 数组字面量**：`static const unsigned char kScript[] = {...}`，
   就是仓库里 `PyUI/examples/` 早先的做法。它要求**每次改脚本都重新编译
   并链接**，而用户机器上只有 `link.exe`，没有编译器。这条路把「改一行
   脚本」变成「装一套 MSVC」。
3. **追加到文件尾部**：Windows 加载器只按节表映射镜像，尾部多余字节完全
   不参与加载。打包就是两次 `open/write`，纯 Python，零外部工具。

选 3。它正好是「用户只装链接器」这条约束的落点——实际上连链接器都不用，
因为 launcher 是预先编好的，打包只需要复制文件。

── 格式 ──

    [launcher.exe 的原始字节]
    [字节码]                      长度写进尾部索引
    [尾部 16 字节]
        magic     "AIPYAPP1"      8 字节
        length    u64 小端         字节码长度

索引放尾部而不是头部：头部是 PE 头与节表的地盘，插一个标记就得重算
所有节的偏移。

── 校验 ──

打包前检查 launcher 自身的尾部**没有**已有索引。不然「对一个已经打包过的
exe 再打包」会产出双层嵌套，而它仍然能跑（读最内层），症状是「改了脚本
但输出没变」——这类静默错误最难查。
"""

import io
import os
import struct

MAGIC = b'AIPYAPP1'
TRAILER_SIZE = 16


class BundleError(Exception):
    """打包失败的原因，直接给命令行用户看。"""


def read_trailer(path):
    """读尾部索引。没有索引返回 None（说明这是裸 launcher）。"""
    size = os.path.getsize(path)
    if size < TRAILER_SIZE:
        return None
    with io.open(path, 'rb') as handle:
        handle.seek(size - TRAILER_SIZE)
        trailer = handle.read(TRAILER_SIZE)
    if trailer[:8] != MAGIC:
        return None
    length = struct.unpack('<Q', trailer[8:])[0]
    if length == 0 or length > size - TRAILER_SIZE:
        # 长度越界：文件被截断过。当作没有索引，让调用方报清晰的错误，
        # 而不是按一个错误的偏移去读。
        return None
    return length


def is_bundled(path):
    return read_trailer(path) is not None


def bundle(launcher, bytecode, output):
    """把 bytecode 追加到 launcher 后面，写到 output。返回产物大小。"""
    if not os.path.isfile(launcher):
        raise BundleError('找不到 launcher：%s' % launcher)
    if not os.path.isfile(bytecode):
        raise BundleError('找不到字节码：%s' % bytecode)

    if is_bundled(launcher):
        raise BundleError(
            '%s 已经是打包产物。请用 CMake 产出的原始 aipy_app.exe 作 launcher，'
            '否则会套两层——产物仍能运行，但读的是内层旧脚本。' % launcher)

    payload = io.open(bytecode, 'rb').read()
    if not payload:
        raise BundleError('%s 是空文件' % bytecode)
    if payload[:8] != b'AIPYBC01':
        raise BundleError('%s 不是 AiPy 字节码（magic 不匹配）' % bytecode)

    # 先写临时文件再改名：中途失败不会留下一个「看起来像 exe、实际截断」
    # 的产物。Windows 上直接覆盖正在运行的同名 exe 会失败，改名也顺带
    # 避开了这一点。
    temp = output + '.tmp'
    with io.open(temp, 'wb') as out:
        with io.open(launcher, 'rb') as src:
            while True:
                chunk = src.read(1 << 20)
                if not chunk:
                    break
                out.write(chunk)
        out.write(payload)
        out.write(MAGIC)
        out.write(struct.pack('<Q', len(payload)))
    os.replace(temp, output)
    return os.path.getsize(output)


def extract(path, output):
    """从打包产物里取出字节码，写回 .bc。调试与校验用。"""
    length = read_trailer(path)
    if length is None:
        raise BundleError('%s 里没有内嵌脚本' % path)
    size = os.path.getsize(path)
    with io.open(path, 'rb') as handle:
        handle.seek(size - TRAILER_SIZE - length)
        payload = handle.read(length)
    with io.open(output, 'wb') as handle:
        handle.write(payload)
    return len(payload)


if __name__ == '__main__':
    import sys

    sys.exit('这是库，请用 aipyic.py --exe 或 --unbundle')
