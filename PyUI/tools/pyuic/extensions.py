#!/usr/bin/env python3
# 扩展机制：把 Python 里的 import 变成 C++ 的静态链接。
#
# ── 为什么要单独一个文件 ──
#
# 定位是「核心只提供最基础的 Python 语义，其余能力全部由扩展提供」。
# 于是 import 不再是禁止项，而是**唯一的扩展入口**。这条边界必须只有一处
# 定义，否则「哪些 import 合法、各自链哪个库」会散落到生成器和 CMake 两边，
# 迟早对不上。
#
# ── 为什么用清单文件而不是扫 C++ 头 ──
#
# 从 C++ 头里反推「Python 该看到什么」需要解析 C++，那是另一个编译器。
# 清单是显式的：扩展作者写清楚「这个模块叫什么、函数映射到哪个符号、
# 需要几个参数」，pyuic 只做查表。代价是多一个文件，换来的是
#   * 构建期就能报「这个函数不存在」并带行号；
#   * 参数个数在编译期校验，而不是等到运行期默默传错；
#   * 链接哪个库完全由清单决定，不需要猜。
#
# ── 清单格式（JSON）──
#
#   {
#     "module":  "fs",                      # Python 里 import 的名字
#     "header":  "pyui/ext/fs.h",           # 生成的代码 include 它
#     "library": "pyui_ext_fs",             # CMake 目标名，用于链接
#     "doc":     "文件读写与路径拼接",         # 报错时给人看
#     "functions": [
#       {"python": "read_file", "cpp": "pyui::ext::fs::readFile", "arity": [1, 1]}
#     ]
#   }
#
# arity 是 [最少, 最多]，与 pyuic 里 FREE_FUNCTIONS 的约定一致。
import json
import os


class ExtensionError(Exception):
    """清单本身写错了（不是脚本写错）。""" 


class Extension:
    """一个扩展模块。"""

    def __init__(self, module, header, library, functions, manifest_path, doc=''):
        self.module = module
        self.header = header
        self.library = library
        # python 名 → (cpp 符号, 最少参数, 最多参数)
        self.functions = functions
        self.manifest_path = manifest_path
        self.doc = doc

    def describe(self):
        return '%s（%s）' % (self.module, self.doc) if self.doc else self.module


def _require(mapping, key, path):
    value = mapping.get(key)
    if not isinstance(value, str) or not value:
        raise ExtensionError('%s: 缺少字符串字段 "%s"' % (path, key))
    return value


def load_manifest(path):
    """读一个 extension.json。任何结构错误都当场抛出，带文件名。"""
    try:
        with open(path, 'r', encoding='utf-8-sig') as handle:
            data = json.load(handle)
    except OSError as error:
        raise ExtensionError('%s: 打不开：%s' % (path, error))
    except ValueError as error:
        raise ExtensionError('%s: 不是合法 JSON：%s' % (path, error))

    if not isinstance(data, dict):
        raise ExtensionError('%s: 顶层必须是对象' % path)

    module = _require(data, 'module', path)
    header = _require(data, 'header', path)
    library = _require(data, 'library', path)
    doc = data.get('doc', '') if isinstance(data.get('doc', ''), str) else ''

    entries = data.get('functions')
    if not isinstance(entries, list) or not entries:
        raise ExtensionError('%s: "functions" 必须是非空数组' % path)

    functions = {}
    for position, entry in enumerate(entries):
        where = '%s: functions[%d]' % (path, position)
        if not isinstance(entry, dict):
            raise ExtensionError('%s: 必须是对象' % where)
        python_name = _require(entry, 'python', where)
        cpp_name = _require(entry, 'cpp', where)
        arity = entry.get('arity')
        if (not isinstance(arity, list) or len(arity) != 2
                or not all(isinstance(v, int) and v >= 0 for v in arity)
                or arity[0] > arity[1]):
            raise ExtensionError('%s: "arity" 必须是 [最少, 最多] 两个非负整数，'
                                 '且前者不大于后者' % where)
        if python_name in functions:
            raise ExtensionError('%s: 函数 "%s" 重复声明' % (where, python_name))
        functions[python_name] = (cpp_name, arity[0], arity[1])

    return Extension(module, header, library, functions, path, doc)


def discover(search_dirs):
    """在若干目录下找 <dir>/*/extension.json。找不到就跳过，不报错。"""
    found = []
    for directory in search_dirs:
        if not directory or not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            candidate = os.path.join(directory, name, 'extension.json')
            if os.path.isfile(candidate):
                found.append(candidate)
    return found


class ExtensionIndex:
    """全部已知扩展，按模块名索引。"""

    def __init__(self, extensions=()):
        self.by_module = {}
        for extension in extensions:
            if extension.module in self.by_module:
                other = self.by_module[extension.module]
                raise ExtensionError(
                    '模块名 "%s" 被两个清单重复定义：\n  %s\n  %s'
                    % (extension.module, other.manifest_path, extension.manifest_path))
            self.by_module[extension.module] = extension

    def __len__(self):
        return len(self.by_module)

    def find(self, module):
        return self.by_module.get(module)

    def module_names(self):
        return sorted(self.by_module)

    def describe_available(self):
        """报错时列给用户看：有哪些模块可用、各提供什么。"""
        if not self.by_module:
            return '（当前没有任何扩展。加一个扩展就是在 ext/ 下放一个 extension.json）'
        lines = []
        for name in sorted(self.by_module):
            extension = self.by_module[name]
            functions = '、'.join(sorted(extension.functions))
            lines.append('  %-12s %s%s' % (name, extension.describe(),
                                           ('：' + functions) if functions else ''))
        return '\n'.join(lines)


def write_link_manifest(path, libraries):
    """生成给 CMake include 的链接清单。

    为什么由 pyuic 生成而不是在 CMakeLists 里手写：链接哪些库必须与脚本
    实际 import 的模块一致。手写两份就会漂移——脚本删了 import、库还留着
    （体积白涨），或者脚本加了 import、库忘了加（链接失败）。让生成器
    同时决定「编什么」和「链什么」，两者不可能不一致。

    即使没有任何扩展也写一个空文件：CMake 的 OUTPUT 列表必须稳定，否则
    每次构建都要重新 configure。
    """
    lines = [
        '# 由 pyuic 生成，请勿手工修改。',
        '# 本文件列出脚本 import 的扩展所对应的 CMake 目标。',
        '',
    ]
    if libraries:
        lines.append('set(PYUI_LINKED_EXTENSIONS %s)' % ' '.join(libraries))
    else:
        lines.append('set(PYUI_LINKED_EXTENSIONS)')
    lines.append('')
    content = '\n'.join(lines)

    try:
        with open(path, 'r', encoding='utf-8', newline='') as handle:
            if handle.read() == content:
                return
    except OSError:
        pass
    with open(path, 'w', encoding='utf-8', newline='') as handle:
        handle.write(content)
