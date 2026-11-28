#!/usr/bin/env python3
# 扩展清单的读取：把 Python 里的 import 解析成 C++ 的调用与链接。
#
# ── 与 pylib.py 的关系 ──
#
# 清单格式只有一套，定义在 pylib.py 的 manifest.json（format=1）。这里只是它的
# **读端**：pylib.py 负责打包与校验，extensions.py 负责在 pyuic 里查表。
# 早先这里有一套独立的 extension.json（"library" 给 CMake 目标名），已经废除：
# 用户机器上没有 CMake，目标名无法解析成文件，那个字段在第一个人用的时候就
# 会暴露成「链接失败」。现在只认 native.lib{平台: 包内路径}。
#
# ── 为什么 pyuic 还要读清单 ──
#
# 代码生成需要「Python 函数名 → C++ 符号」的映射，这一条信息只能在清单里。
# 链接清单（哪些 .pylib）由 C++ 的 pyui_extc 算，这里不碰——分工是：
#   pyuic       有 AST 和行号，负责「记录用了什么」
#   pyui_extc   纯数据加工，负责「按清单决定链接什么」
import json
import os


class ExtensionError(Exception):
    """清单本身写错了（不是脚本写错）。"""


class Extension:
    """一个扩展模块。"""

    def __init__(self, module, header, includes, functions, manifest_path, libs,
                 depends, doc=''):
        self.module = module
        # 生成的代码 include 它。空表示纯 Python 扩展（只有字节码）。
        self.header = header
        # 编译期需要的 include 目录（相对包根）
        self.includes = includes
        self.functions = functions  # python 名 → (cpp 符号, 最少, 最多)
        self.manifest_path = manifest_path
        self.libs = libs            # 平台 → 包内相对路径
        self.depends = depends
        self.doc = doc

    def describe(self):
        return '%s（%s）' % (self.module, self.doc) if self.doc else self.module


def _text(mapping, key, where, required=True):
    value = mapping.get(key)
    if value is None:
        if required:
            raise ExtensionError('%s: 缺少字符串字段 "%s"' % (where, key))
        return ''
    if not isinstance(value, str) or (required and not value):
        raise ExtensionError('%s: "%s" 必须是非空字符串' % (where, key))
    return value


def _arity(entry, where):
    value = entry.get('arity')
    if (not isinstance(value, list) or len(value) != 2
            or not all(isinstance(v, int) and not isinstance(v, bool) for v in value)):
        raise ExtensionError('%s: "arity" 必须是 [最少, 最多] 两个整数' % where)
    low, high = value
    if low < 0 or high < low:
        raise ExtensionError('%s: "arity" 必须满足 0 <= 最少 <= 最多' % where)
    return low, high


def load_manifest(path):
    """读一个 manifest.json。任何结构错误都当场抛出，带文件名。"""
    try:
        with open(path, 'r', encoding='utf-8-sig') as handle:
            data = json.load(handle)
    except OSError as error:
        raise ExtensionError('%s: 打不开：%s' % (path, error))
    except ValueError as error:
        raise ExtensionError('%s: 不是合法 JSON：%s' % (path, error))

    if not isinstance(data, dict):
        raise ExtensionError('%s: 顶层必须是对象' % path)
    if data.get('format') != 1:
        raise ExtensionError('%s: 不支持的 format=%r（本工具只认 1）'
                             % (path, data.get('format')))

    modules = data.get('modules')
    if not isinstance(modules, list) or not modules:
        raise ExtensionError('%s: "modules" 必须是非空数组' % path)

    result = []
    for position, entry in enumerate(modules):
        where = '%s: modules[%d]' % (path, position)
        if not isinstance(entry, dict):
            raise ExtensionError('%s: 必须是对象' % where)

        module = _text(entry, 'name', where)
        doc = _text(entry, 'doc', where, required=False)
        header = _text(entry, 'header', where, required=False)

        functions = {}
        entries = entry.get('functions') or []
        if not isinstance(entries, list):
            raise ExtensionError('%s: "functions" 必须是数组' % where)
        for index, item in enumerate(entries):
            item_where = '%s.functions[%d]' % (where, index)
            if not isinstance(item, dict):
                raise ExtensionError('%s: 必须是对象' % item_where)
            python_name = _text(item, 'python', item_where)
            cpp_name = _text(item, 'cpp', item_where)
            low, high = _arity(item, item_where)
            if python_name in functions:
                raise ExtensionError('%s: 函数 "%s" 重复声明'
                                     % (item_where, python_name))
            functions[python_name] = (cpp_name, low, high)

        native = entry.get('native') or {}
        if not isinstance(native, dict):
            raise ExtensionError('%s: "native" 必须是对象' % where)
        includes = native.get('include') or []
        if not isinstance(includes, list) or not all(isinstance(p, str) for p in includes):
            raise ExtensionError('%s.native: "include" 必须是路径数组' % where)
        libs = native.get('lib') or {}
        if not isinstance(libs, dict):
            raise ExtensionError('%s.native: "lib" 必须是 {平台: 路径}' % where)
        for platform, lib_path in libs.items():
            if not isinstance(lib_path, str) or not lib_path:
                raise ExtensionError('%s.native.lib."%s": 必须是包内路径'
                                     % (where, platform))

        depends = entry.get('depends') or []
        if not isinstance(depends, list) or not all(isinstance(d, str) for d in depends):
            raise ExtensionError('%s: "depends" 必须是模块名数组' % where)

        if not header and not functions and not libs:
            raise ExtensionError('%s: 模块 "%s" 既没有 header、函数或静态库'
                                 % (where, module))

        result.append(Extension(module, header, list(includes), functions, path,
                                dict(libs), list(depends), doc))
    return result


def discover(search_dirs):
    """在若干目录下找 <dir>/*/manifest.json。找不到就跳过，不报错。

    也接受直接把 <dir> 指向某个包目录（里面就是 manifest.json）。
    """
    found = []
    for directory in search_dirs:
        if not directory or not os.path.isdir(directory):
            continue
        direct = os.path.join(directory, 'manifest.json')
        if os.path.isfile(direct):
            found.append(direct)
            continue
        for name in sorted(os.listdir(directory)):
            candidate = os.path.join(directory, name, 'manifest.json')
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
                    % (extension.module, other.manifest_path,
                       extension.manifest_path))
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
            return '（当前没有任何扩展。加一个扩展就是在 ext/ 下放一个 manifest.json）'
        lines = []
        for name in sorted(self.by_module):
            extension = self.by_module[name]
            functions = '、'.join(sorted(extension.functions))
            lines.append('  %-12s %s%s' % (name, extension.describe(),
                                           ('：' + functions) if functions else ''))
        return '\n'.join(lines)


def load_dirs(search_dirs):
    """从若干目录发现并装载全部扩展。清单有错就抛 ExtensionError。"""
    extensions = []
    for path in discover(search_dirs):
        extensions.extend(load_manifest(path))
    return ExtensionIndex(extensions)
