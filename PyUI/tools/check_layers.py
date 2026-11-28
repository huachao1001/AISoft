#!/usr/bin/env python3
"""分层守护：PyUI（界面）不得反向依赖 AiPy 的实现细节。

分层已经不是「同一个仓库里两个 target」了：语言运行时是**独立项目**
`../AiPy`，PyUI 通过 `add_subdirectory` 消费它。所以这里守的东西变了：

1. PyUI 的界面源文件不许 include `aipy/` 的头（只能用 `pyui/` 的转发头）。
   转发头本身是「使用」，直接 include `aipy/value.h` 会绕过那层收口。
2. PyUI 侧不许再出现 `Value` 的实现文件——它属于 AiPy。出现就说明有人
   复制了一份回来，两份会各自漂移。
3. `pyui/utf8.h`、`pyui/py_value.h` 必须是转发头，不许有函数定义。
4. AiPy 那边必须零界面依赖（扫它的源文件）。
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AIPY = os.path.normpath(os.path.join(ROOT, '..', 'AiPy'))

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.MULTILINE)

# 界面源文件允许 include 的 aipy 头：一个都不许。
FORBIDDEN_IN_UI = ('aipy/',)

# AiPy 里不许出现的界面符号。只查真正的符号，不管注释里提到的词。
UI_SYMBOL_RE = re.compile(
    r'\b(?:'
    r'pyui::(?!py\b)'          # pyui:: 但排除 pyui::py（历史调用点没有）
    r'|View\b|Window\b|Widget\b|Painter\b|Surface\b|HWND\b|HDC\b'
    r'|Theme\b|LayoutInflater\b'
    r')')

# 注释行不参与符号扫描：解释「为什么不依赖界面」时当然会提到界面词。
COMMENT_RE = re.compile(r'^\s*//')


def read(path):
    with open(path, 'r', encoding='utf-8') as handle:
        return handle.read()


def cmake_ui_sources():
    """从 CMakeLists 读出 PYUI_SOURCES（set 与 list(APPEND) 都要）。"""
    text = read(os.path.join(ROOT, 'CMakeLists.txt'))
    chunks = re.findall(r'set\(PYUI_SOURCES(.*?)\)', text, re.DOTALL)
    chunks += re.findall(r'list\(APPEND\s+PYUI_SOURCES(.*?)\)', text, re.DOTALL)
    sources = []
    for chunk in chunks:
        sources.extend(item.strip() for item in chunk.split()
                       if item.strip().endswith('.cpp'))
    return sources


def check_ui_does_not_include_aipy():
    failures = []
    for rel in cmake_ui_sources():
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            failures.append('%s: PYUI_SOURCES 里列了但文件不存在' % rel)
            continue
        for name in INCLUDE_RE.findall(read(path)):
            if any(name.startswith(prefix) for prefix in FORBIDDEN_IN_UI):
                failures.append(
                    '%s: 界面层直接 include 了 %s；应通过 pyui/ 的转发头使用'
                    % (rel, name))
    return failures


def check_no_copied_implementation():
    """实现文件必须只在 AiPy 里。PyUI 留副本就等于两份实现同时存在。"""
    failures = []
    for rel in ('src/py_value.cpp', 'src/utf8.cpp'):
        if os.path.exists(os.path.join(ROOT, rel)):
            failures.append(
                '%s: 语言层实现不该留在 PyUI（它属于 AiPy）' % rel)
    return failures


def check_forward_headers():
    """转发头里不许有函数定义——有定义就说明实现又搬回来了。"""
    failures = []
    for rel in ('include/pyui/utf8.h', 'include/pyui/py_value.h'):
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            failures.append('%s: 转发头不见了' % rel)
            continue
        text = read(path)
        if 'aipy/' not in text:
            failures.append('%s: 应该转发到 aipy/ 的头' % rel)
        # 函数定义的特征：形如 `size_t utf8Length(...) {` 的行
        if re.search(r'\)\s*\{', text):
            failures.append('%s: 转发头里出现了函数定义' % rel)
    return failures


def check_cmake_wires_aipy():
    text = read(os.path.join(ROOT, 'CMakeLists.txt'))
    failures = []
    if 'add_subdirectory(${AIPY_DIR}' not in text:
        failures.append('CMakeLists.txt: 没有通过 add_subdirectory 引入同级 AiPy')
    if 'aipy_base' not in text:
        failures.append('CMakeLists.txt: pyui_core 没有链接 aipy_base（utf8 在那边）')
    for stale in ('add_library(aipy_core', 'add_library(pyui_base'):
        if stale in text:
            failures.append('CMakeLists.txt: 还在本地定义 %s' % stale)
    return failures


def check_aipy_is_ui_free():
    """AiPy 是独立项目，它自己也不许碰界面。"""
    if not os.path.isdir(AIPY):
        return ['找不到同级 AiPy 项目：%s' % AIPY]
    failures = []
    for folder in ('include', 'src'):
        base = os.path.join(AIPY, folder)
        for dirpath, _, filenames in os.walk(base):
            for name in filenames:
                if not name.endswith(('.h', '.cpp')):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, AIPY).replace('\\', '/')
                for number, line in enumerate(read(path).splitlines(), 1):
                    if COMMENT_RE.match(line):
                        continue
                    if UI_SYMBOL_RE.search(line):
                        failures.append('AiPy/%s:%d: 出现界面符号 -> %s'
                                        % (rel, number, line.strip()))
    return failures


def main():
    failures = []
    for check in (check_ui_does_not_include_aipy, check_no_copied_implementation,
                  check_forward_headers, check_cmake_wires_aipy,
                  check_aipy_is_ui_free):
        failures.extend(check())
    if failures:
        for line in failures:
            print('layers: %s' % line, file=sys.stderr)
        print('layers: %d 处越界' % len(failures), file=sys.stderr)
        return 1
    print('layers check OK: PyUI 只消费 AiPy，AiPy 零界面依赖')
    return 0


if __name__ == '__main__':
    sys.exit(main())
