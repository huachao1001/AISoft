#!/usr/bin/env python3
# .pylib 包格式：本质是 zip，类比 Android 的 AAR。
#
# ── 它解决什么 ──
#
# 定位是「核心只提供基础 Python 语义，其余能力全部由扩展提供」。扩展一旦
# 成为产品的一部分，就必须有可分发的形态：一个文件发出去，别人解压都不用，
# 直接 `import fs` 就能用。
#
# ── 为什么是 zip ──
#
# 不需要发明格式：zip 有 CRC32 校验、有目录索引、有成熟的工具能看内容，
# 而且 Python 标准库直接读写，构建期不引入任何依赖。
#
# ── 为什么它不是一个「运行期」格式 ──
#
# .pylib 只在**构建期**被打开：解出静态库交给链接器，解出字节码嵌进 exe。
# 产物里没有 zip 解析代码，也没有包查找逻辑——启动零成本。
# 这一点必须写清楚，否则很容易「顺手」在运行期加一个解包器，把体积和
# 启动时间都赔进去。
#
# ── 目录结构 ──
#
#   <name>.pylib
#   ├── manifest.json               # 模块名、版本、函数表
#   ├── bytecode/<module>.pybc      # 可选：纯 Python 模块编译出的字节码
#   ├── include/...                 # 可选：扩展的 C++ 头
#   └── lib/<platform>/<name>.lib   # 可选：静态库
#
# 包内静态库用标准后缀（MSVC 的 .lib、GCC 的 .a），因为包是 zip、
# 解包就是对真实文件，没有「消费者要靠扩展名猜编译器」的问题。
#
# 一个模块可以只有字节码（纯 Python 扩展）、只有静态库（纯原生扩展）、
# 或两者都有（Python 包装 + 原生实现）。
#
# ── 用法 ──
#
#   python tools/pyuic/pylib.py pack   <源目录> -o <输出.pylib>
#   python tools/pyuic/pylib.py list   <某个.pylib>
#   python tools/pyuic/pylib.py verify <某个.pylib>
#   python tools/pyuic/pylib.py unpack <某个.pylib> -d <目录>
import argparse
import hashlib
import json
import os
import sys
import zipfile


FORMAT_VERSION = 1
MANIFEST_NAME = 'manifest.json'

# 支持的平台标识。与构建脚本用的三元组保持一致，避免两边各写一套名字。
# 目前只有 Windows 是已实现的后端（见 DESIGN.md 第 8 节）。
KNOWN_PLATFORMS = ('win-x64', 'win-arm64', 'mac-arm64', 'linux-x64')

# 包内静态库的后缀跟随平台惯例：MSVC 用 .lib，GCC/Clang 用 .a。
#
# 为什么不再统一改成别的后缀：包本身已经是 .pylib（一个 zip），解包得到的是
# 真实文件；这一步之后就看不出「这是从包里解出来的」，没有理由让库文件伪装成
# 另一种名字。而包名与库名用同一个 .pylib 后缀反而会让「哪个是包、哪个是库」
# 永远需要靠上下文区分。
LIB_SUFFIX = {
    'win-x64': '.lib',
    'win-arm64': '.lib',
    'mac-arm64': '.a',
    'linux-x64': '.a',
}


class PylibError(Exception):
    """包本身有问题（不是使用者写错脚本）。"""


# ─────────────────────────────────────────────────────────────
# 清单校验
# ─────────────────────────────────────────────────────────────

def _check_arity(value, where):
    if (not isinstance(value, list) or len(value) != 2
            or not all(isinstance(v, int) and v >= 0 for v in value)
            or value[0] > value[1]):
        raise PylibError('%s: "arity" 必须是 [最少, 最多] 两个非负整数，'
                       '且前者不大于后者' % where)
    return (value[0], value[1])


def validate_manifest(data, source='manifest.json'):
    """校验清单结构。返回规范化后的 dict；任何问题都抛 PylibError。

    校验放在这里而不是各处调用点：清单是**跨团队契约**，错误信息必须
    指向具体字段。散落的 `.get()` 会让「少了 header」变成一个 None
    传进链接器，报错完全不相关。
    """
    if not isinstance(data, dict):
        raise PylibError('%s: 顶层必须是对象' % source)

    version = data.get('format')
    if version != FORMAT_VERSION:
        raise PylibError('%s: 不支持的 format=%r（本工具只认 %d）。'
                       '包版本与新工具不匹配时，请用对应版本的工具打包'
                       % (source, version, FORMAT_VERSION))

    name = data.get('name')
    if not isinstance(name, str) or not name:
        raise PylibError('%s: 缺少 "name"' % source)

    modules = data.get('modules')
    if not isinstance(modules, list) or not modules:
        raise PylibError('%s: "modules" 必须是非空数组' % source)

    seen = set()
    normalized = []
    for index, module in enumerate(modules):
        where = '%s: modules[%d]' % (source, index)
        if not isinstance(module, dict):
            raise PylibError('%s: 必须是对象' % where)

        module_name = module.get('name')
        if not isinstance(module_name, str) or not module_name:
            raise PylibError('%s: 缺少 "name"' % where)
        if module_name in seen:
            raise PylibError('%s: 模块名 "%s" 重复' % (where, module_name))
        seen.add(module_name)

        bytecode = module.get('bytecode')
        if bytecode is not None and (not isinstance(bytecode, str) or not bytecode):
            raise PylibError('%s: "bytecode" 必须是包内路径' % where)

        functions = {}
        entries = module.get('functions') or []
        if not isinstance(entries, list):
            raise PylibError('%s: "functions" 必须是数组' % where)
        for position, entry in enumerate(entries):
            entry_where = '%s.functions[%d]' % (where, position)
            if not isinstance(entry, dict):
                raise PylibError('%s: 必须是对象' % entry_where)
            python_name = entry.get('python')
            cpp_name = entry.get('cpp')
            if not isinstance(python_name, str) or not python_name:
                raise PylibError('%s: 缺少 "python"' % entry_where)
            if not isinstance(cpp_name, str) or not cpp_name:
                raise PylibError('%s: 缺少 "cpp"' % entry_where)
            if python_name in functions:
                raise PylibError('%s: 函数 "%s" 重复' % (entry_where, python_name))
            functions[python_name] = (cpp_name, _check_arity(entry.get('arity'), entry_where))

        native = module.get('native') or {}
        if not isinstance(native, dict):
            raise PylibError('%s: "native" 必须是对象' % where)

        include = native.get('include') or []
        if not isinstance(include, list) or not all(isinstance(p, str) for p in include):
            raise PylibError('%s.native: "include" 必须是路径数组' % where)

        libs = native.get('lib') or {}
        if not isinstance(libs, dict):
            raise PylibError('%s.native: "lib" 必须是 {平台: 路径}' % where)
        for platform, path in libs.items():
            if platform not in KNOWN_PLATFORMS:
                raise PylibError('%s.native.lib: 未知平台 "%s"（可用：%s）'
                               % (where, platform, '、'.join(KNOWN_PLATFORMS)))
            if not isinstance(path, str) or not path:
                raise PylibError('%s.native.lib."%s": 必须是包内路径' % (where, platform))
            # 后缀按平台惯例：Windows 是 .lib，macOS/Linux 是 .a。
            if not path.lower().endswith(LIB_SUFFIX[platform]):
                raise PylibError('%s.native.lib."%s": 静态库必须以 %s 结尾（当前是 "%s"）。'
                               '包内库用各平台的原生后缀：Windows 是 .lib，'
                               'macOS/Linux 是 .a'
                               % (where, platform, LIB_SUFFIX[platform], path))

        if bytecode is None and not functions and not libs:
            raise PylibError('%s: 模块 "%s" 既没有字节码、也没有函数或静态库，'
                           '这样的包解出来是空的' % (where, module_name))

        normalized.append({
            'name': module_name,
            'doc': module.get('doc', '') if isinstance(module.get('doc', ''), str) else '',
            'bytecode': bytecode,
            'functions': functions,
            'include': list(include),
            'lib': dict(libs),
        })

    return {
        'format': FORMAT_VERSION,
        'name': name,
        'version': data.get('version', '') if isinstance(data.get('version', ''), str) else '',
        'description': (data.get('description', '')
                        if isinstance(data.get('description', ''), str) else ''),
        'modules': normalized,
    }


# ─────────────────────────────────────────────────────────────
# 读包
# ─────────────────────────────────────────────────────────────

class Package:
    """一个已打开（已校验）的 .pylib。

    刻意不保留 open 的 zip 句柄：读完清单就关。构建期读它的都是短操作，
    长期持有句柄只会在 Windows 上阻止别人覆盖文件。
    """

    def __init__(self, path, manifest, checksum):
        self.path = path
        self.manifest = manifest
        self.checksum = checksum
        self.digest = hashlib.sha256(checksum).hexdigest()[:16]

    @property
    def name(self):
        return self.manifest['name']

    def module(self, name):
        for module in self.manifest['modules']:
            if module['name'] == name:
                return module
        return None

    def module_names(self):
        return [m['name'] for m in self.manifest['modules']]

    def has_platform(self, platform):
        """包里是否有当前平台可用的静态库。纯 Python 包永远为真。"""
        for module in self.manifest['modules']:
            if module['lib'] and platform not in module['lib']:
                # 有原生部分但缺这个平台：这个包在目标平台上不可用
                return False
        return True

    def libraries(self, platform):
        """当前平台要链接的静态库（包内路径）。"""
        found = []
        for module in self.manifest['modules']:
            path = module['lib'].get(platform)
            if path:
                found.append((module['name'], path))
        return found


def read_manifest(path):
    """只读清单，不读整个包。给「探测有哪些模块」这种轻量场景用。"""
    try:
        with zipfile.ZipFile(path) as archive:
            try:
                raw = archive.read(MANIFEST_NAME)
            except KeyError:
                raise PylibError('%s: 不是 pyui 扩展包（缺少 %s）'
                               % (path, MANIFEST_NAME))
    except zipfile.BadZipFile:
        raise PylibError('%s: 不是合法的 zip（.pylib 应是 zip 格式）' % path)
    except OSError as error:
        raise PylibError('%s: 打不开：%s' % (path, error))

    try:
        data = json.loads(raw.decode('utf-8-sig'))
    except (UnicodeDecodeError, ValueError) as error:
        raise PylibError('%s: %s 不是合法 JSON：%s' % (path, MANIFEST_NAME, error))
    return validate_manifest(data, path)


def open_package(path):
    """完整校验：清单合法 + 清单里提到的每个文件都真的在包里。

    为什么要在这里就把「文件缺失」查出来：如果留到链接期，症状会是
    「LNK1104 无法打开 fs.lib」——使用者完全不会想到是包打少了文件。
    """
    try:
        with zipfile.ZipFile(path, 'r') as archive:
            names = set(archive.namelist())
            try:
                raw = archive.read(MANIFEST_NAME)
            except KeyError:
                raise PylibError('%s: 不是 pyui 扩展包（缺少 %s）'
                               % (path, MANIFEST_NAME))
            checksum = archive.read(MANIFEST_NAME)
    except zipfile.BadZipFile:
        raise PylibError('%s: 不是合法的 zip（.pylib 应是 zip 格式）' % path)
    except OSError as error:
        raise PylibError('%s: 打不开：%s' % (path, error))

    try:
        data = json.loads(raw.decode('utf-8-sig'))
    except (UnicodeDecodeError, ValueError) as error:
        raise PylibError('%s: %s 不是合法 JSON：%s' % (path, MANIFEST_NAME, error))

    manifest = validate_manifest(data, path)

    missing = []
    for module in manifest['modules']:
        if module['bytecode'] and module['bytecode'] not in names:
            missing.append('%s 的字节码 %s' % (module['name'], module['bytecode']))
        for platform, lib in module['lib'].items():
            if lib not in names:
                missing.append('%s 的 %s 静态库 %s' % (module['name'], platform, lib))
    if missing:
        raise PylibError('%s: 清单引用的文件不在包里：\n  %s'
                       % (path, '\n  '.join(missing)))

    return Package(path, manifest, checksum)


# ─────────────────────────────────────────────────────────────
# 打包
# ─────────────────────────────────────────────────────────────

# 固定时间戳：同样的输入必须产出逐字节相同的包。
# 否则「内容没变」的包每次构建 mtime 都跳，缓存全失效。
_FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def _add_file(archive, name, data, compress=True):
    info = zipfile.ZipInfo(name, date_time=_FIXED_TIME)
    info.external_attr = 0o644 << 16
    # 静态库本来就压不动，压缩只会白花时间；字节码同理（熵已经很高）。
    info.compress_type = zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED
    archive.writestr(info, data)


def pack(source_dir, output, compress=True):
    """把源目录打成 .pylib。

    源目录必须有一个 manifest.json；清单里引用的相对路径相对于源目录。
    """
    manifest_path = os.path.join(source_dir, MANIFEST_NAME)
    if not os.path.isfile(manifest_path):
        raise PylibError('%s: 缺少 %s' % (source_dir, MANIFEST_NAME))

    with open(manifest_path, 'r', encoding='utf-8-sig') as handle:
        try:
            data = json.load(handle)
        except ValueError as error:
            raise PylibError('%s: 不是合法 JSON：%s' % (manifest_path, error))

    manifest = validate_manifest(data, manifest_path)

    # 清单要原样放进包里，但必须重新序列化：源文件里的缩进、键序、
    # 注释性空白都不该影响包内容。这样「同样语义、不同排版」的清单
    # 得到同一个包。
    payload = dict(data)
    payload['format'] = FORMAT_VERSION
    serialized = json.dumps(payload, ensure_ascii=False, indent=2,
                            sort_keys=True).encode('utf-8') + b'\n'

    # 收集要打包的文件
    entries = [(MANIFEST_NAME, serialized, True)]
    for module in manifest['modules']:
        if module['bytecode']:
            entries.append((module['bytecode'],
                            _read_relative(source_dir, module['bytecode']), compress))
        for path in module['lib'].values():
            # 静态库不压缩：解压开销比省下的几 KB 更值钱
            entries.append((path, _read_relative(source_dir, path), False))
        for include_dir in module['include']:
            root = os.path.join(source_dir, include_dir)
            if not os.path.isdir(root):
                raise PylibError('%s: include 目录不存在：%s' % (module['name'], include_dir))
            for folder, _, files in os.walk(root):
                for filename in sorted(files):
                    absolute = os.path.join(folder, filename)
                    relative = os.path.relpath(absolute, source_dir).replace('\\', '/')
                    entries.append((relative, _read_relative(source_dir, relative), compress))

    # 排序：包内条目顺序会影响 zip 的字节布局，不排序就没有可重现性
    entries.sort(key=lambda item: item[0])

    os.makedirs(os.path.dirname(os.path.abspath(output)) or '.', exist_ok=True)
    with zipfile.ZipFile(output, 'w') as archive:
        for name, data, do_compress in entries:
            _add_file(archive, name, data, do_compress)

    return output


def _read_relative(source_dir, relative):
    absolute = os.path.join(source_dir, relative.replace('/', os.sep))
    if not os.path.isfile(absolute):
        raise PylibError('清单引用的文件不存在：%s' % relative)
    with open(absolute, 'rb') as handle:
        return handle.read()


# ─────────────────────────────────────────────────────────────
# 解包
# ─────────────────────────────────────────────────────────────

def unpack(path, target_dir):
    """解出到目录。构建期用它拿静态库与头文件。"""
    package = open_package(path)
    os.makedirs(target_dir, exist_ok=True)
    with zipfile.ZipFile(path) as archive:
        for name in archive.namelist():
            if name.endswith('/'):
                continue
            # 防目录穿越：包可能来自别人，'../' 不能逃出解包目录
            destination = os.path.normpath(os.path.join(target_dir, name))
            root = os.path.normpath(target_dir)
            if not destination.startswith(root + os.sep) and destination != root:
                raise PylibError('%s: 包内路径越界：%s' % (path, name))
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            with archive.open(name) as source:
                with open(destination, 'wb') as handle:
                    handle.write(source.read())
    return package


# ─────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────

def _describe(package):
    lines = ['包：%s  (%s)' % (package.path, package.name)]
    if package.manifest['version']:
        lines.append('版本：%s' % package.manifest['version'])
    if package.manifest['description']:
        lines.append('说明：%s' % package.manifest['description'])
    lines.append('校验：manifest sha256[:16] = %s' % package.digest)
    lines.append('模块：')
    for module in package.manifest['modules']:
        parts = []
        if module['bytecode']:
            parts.append('字节码')
        if module['functions']:
            parts.append('%d 个函数' % len(module['functions']))
        if module['lib']:
            parts.append('静态库 %s' % '、'.join(sorted(module['lib'])))
        lines.append('  %-10s %s' % (module['name'], '，'.join(parts) or '（空）'))
        if module['functions']:
            names = '、'.join(sorted(module['functions']))
            lines.append('             %s' % names)
    return '\n'.join(lines)


def main(argv):
    parser = argparse.ArgumentParser(prog='pylib', description='.pylib 扩展包工具')
    sub = parser.add_subparsers(dest='command', required=True)

    pack_cmd = sub.add_parser('pack', help='把源目录打成 .pylib')
    pack_cmd.add_argument('source', help='含 manifest.json 的目录')
    pack_cmd.add_argument('-o', '--output', required=True, help='输出 .pylib')

    list_cmd = sub.add_parser('list', help='显示包内容')
    list_cmd.add_argument('package')

    verify_cmd = sub.add_parser('verify', help='校验包结构')
    verify_cmd.add_argument('package')

    unpack_cmd = sub.add_parser('unpack', help='解包到目录')
    unpack_cmd.add_argument('package')
    unpack_cmd.add_argument('-d', '--directory', required=True)

    args = parser.parse_args(argv)

    try:
        if args.command == 'pack':
            output = pack(args.source, args.output)
            print('已生成 %s' % output)
            return 0
        if args.command == 'list':
            print(_describe(open_package(args.package)))
            return 0
        if args.command == 'verify':
            package = open_package(args.package)
            print('OK: %s（%d 个模块）'
                  % (args.package, len(package.manifest['modules'])))
            return 0
        if args.command == 'unpack':
            package = unpack(args.package, args.directory)
            print('已解出 %s -> %s' % (package.name, args.directory))
            return 0
    except PylibError as error:
        print('pylib: %s' % error, file=sys.stderr)
        return 1
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
