#!/usr/bin/env python3
"""AiPy 自检：本项目不认识界面。

为什么这一段必须留在 AiPy 自己身上：它的立身之本就是「单独拿走也能用」。
如果这条边界只由消费方（PyUI）检查，那么有人把 AiPy 整个目录复制到另一个
仓库时就失去了保护——而那正是最需要它的场景。

检查两件事：

1. `include/`、`src/` 下不许出现界面符号。允许 `Host`（`void*`），因为那是
   宿主对象的不透明句柄，AiPy 不解引用它。
2. `Host` 必须是 `void*`。如果哪天变成某个具体类型，就说明这条边界被
   悄悄换成了「认识一种特定宿主」。
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 界面符号。只匹配真正的标识符，注释里的说明文字不算。
UI_SYMBOL_RE = re.compile(
    r'\b(?:'
    r'View|Window|Widget|Painter|Surface|Renderer|Layout'
    r'|HWND|HDC|HBITMAP|HBRUSH|COLORREF'
    r'|Theme|Color|Rect|Point'
    r')\b')

COMMENT_RE = re.compile(r'^\s*//')
BLOCK_COMMENT_START = re.compile(r'^\s*/\*')
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')

# 这些头里出现 `Rect`/`Point` 之类是标准库或本项目自己的词，不误报：
# value.h 里的注释用词已排除，utf8.h 只有 size_t。
ALLOW_COMMENT_ONLY = True


def read(path):
    with open(path, 'r', encoding='utf-8') as handle:
        return handle.read()


def check_no_ui_symbols():
    failures = []
    for folder in ('include', 'src'):
        base = os.path.join(ROOT, folder)
        for dirpath, _, filenames in os.walk(base):
            for name in sorted(filenames):
                if not name.endswith(('.h', '.cpp')):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, ROOT).replace('\\', '/')
                in_block = False
                for number, line in enumerate(read(path).splitlines(), 1):
                    stripped = line.strip()
                    if in_block:
                        if '*/' in stripped:
                            in_block = False
                        continue
                    if BLOCK_COMMENT_START.match(line):
                        in_block = '*/' not in stripped
                        continue
                    # 行尾注释之前的部分才参与符号扫描
                    code = stripped.split('//')[0]
                    if not code:
                        continue
                    match = UI_SYMBOL_RE.search(code)
                    if match:
                        failures.append('%s:%d: 出现界面符号 %r -> %s'
                                        % (rel, number, match.group(0), code))
    return failures


def check_host_is_opaque():
    path = os.path.join(ROOT, 'include', 'aipy', 'types.h')
    if not os.path.isfile(path):
        return ['include/aipy/types.h 不见了：Host 的定义没地方找了']
    text = read(path)
    if not re.search(r'using\s+Host\s*=\s*void\s*\*', text):
        return ['include/aipy/types.h: Host 不再是 void*，'
                'AiPy 开始「认识」某种具体宿主类型了']
    return []


def check_no_pyui_include():
    failures = []
    for folder in ('include', 'src', 'tests'):
        base = os.path.join(ROOT, folder)
        for dirpath, _, filenames in os.walk(base):
            for name in sorted(filenames):
                if not name.endswith(('.h', '.cpp')):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, ROOT).replace('\\', '/')
                for name_included in INCLUDE_RE.findall(read(path)):
                    if name_included.startswith('pyui/'):
                        failures.append('%s: include 了 %s'
                                        % (rel, name_included))
    return failures


def main():
    failures = []
    for check in (check_no_ui_symbols, check_host_is_opaque,
                  check_no_pyui_include):
        failures.extend(check())
    if failures:
        for line in failures:
            print('aipy-layers: %s' % line, file=sys.stderr)
        print('aipy-layers: %d 处越界' % len(failures), file=sys.stderr)
        return 1
    print('aipy-layers OK: 无界面符号，Host 是不透明 void*')
    return 0


if __name__ == '__main__':
    sys.exit(main())
