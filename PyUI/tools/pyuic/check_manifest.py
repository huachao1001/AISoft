#!/usr/bin/env python3
# 清单（manifest.xml）的检查：格式、报错质量、与脚本常量的冲突。
#
# ── 为什么需要这个 ──
#
# 清单是**手写文件**，所以它的价值有一半在「写错时能立刻说清哪里错了」。
# 解析器写对了但报错位置偏了，使用者仍然是在猜——那种体验和没有校验一样。
# 因此这里的用例不只验证「能不能读」，还验证：
#
#   1. 报错必须带位置（行号列号），否则用户没法定位；
#   2. 拼错属性名必须被拒绝，不能静默忽略（静默忽略是「改了没生效」的根源）；
#   3. 类型错误必须拒绝（width="316" 合法，width="abc" 不合法）；
#   4. 认不出的子节点必须拒绝，否则写错的 <windw> 会被当成没写；
#   5. 清单与脚本写同一字段时必须报冲突，而不是悄悄只认一个。
#
# 用法：
#   python tools/pyuic/check_manifest.py
import ast
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pyuic  # noqa: E402


# (名字, 清单文本, 期望)  期望 'ok' 或 'error'
CASES = [
    # ── 合法输入 ──
    ('empty', '<manifest/>', 'ok'),
    ('activity_only', '<manifest><activity name="Main"/></manifest>', 'ok'),
    ('basic',
     '<manifest><activity name="A"><window width="316" height="512"/>'
     '</activity></manifest>', 'ok'),
    ('empty_window',
     '<manifest><activity name="A"><window/></activity></manifest>', 'ok'),
    ('self_closing', '<manifest name="probe"><activity name="A"/></manifest>', 'ok'),
    ('comment', '<!-- 说明 -->\n<manifest><activity name="A"/></manifest>', 'ok'),
    ('prolog', '<?xml version="1.0" encoding="utf-8"?>\n<manifest/>', 'ok'),
    ('bom', '\ufeff<manifest><activity name="A"/></manifest>', 'ok'),
    ('entity',
     '<manifest><activity name="A"><window title="a &amp; b"/></activity></manifest>',
     'ok'),
    ('cdata_like',
     '<manifest><activity name="A"><window title="a &lt;b&gt;"/></activity></manifest>',
     'ok'),
    ('files_on_activity',
     '<manifest><activity name="A" progId="P.Note" description="Note">'
     '<extension>pynote</extension></activity></manifest>', 'ok'),
    ('multi_extension',
     '<manifest><activity name="A"><extension>a</extension>'
     '<extension>b</extension></activity></manifest>', 'ok'),
    ('tray', '<manifest><tray tooltip="hi"/></manifest>', 'ok'),
    ('all_nodes',
     '<manifest name="p"><activity name="A" main="true" progId="P" description="d">'
     '<window title="t" width="100" height="200"/>'
     '<extension>a</extension></activity><tray tooltip="x"/></manifest>', 'ok'),

    # ── 结构错误 ──
    ('not_manifest', '<activity name="A"/>', 'error'),
    ('empty_text', '', 'error'),
    ('unclosed_tag',
     '<manifest><activity name="A"><window width="1"/></activity>', 'error'),
    ('stray_text', '<manifest>oops<activity/></manifest>', 'error'),
    ('two_roots', '<manifest/><manifest/>', 'error'),
    ('window_at_root', '<manifest><window width="1"/></manifest>', 'error'),
    ('files_node_gone', '<manifest><files><extension>a</extension></files></manifest>',
     'error'),
    ('unknown_node', '<manifest><windw width="1"/></manifest>', 'error'),
    ('unknown_root_attr', '<manifest tittle="x"/>', 'error'),
    ('unknown_child_of_activity',
     '<manifest><activity><ext>a</ext></activity></manifest>', 'error'),
    ('empty_extension',
     '<manifest><activity><extension>  </extension></activity></manifest>', 'error'),
    ('two_activities',
     '<manifest><activity name="A"/><activity name="B"/></manifest>', 'error'),
    ('two_main',
     '<manifest><activity name="A" main="true"/>'
     '<activity name="B" main="true"/></manifest>', 'error'),
    ('two_with_main_ok',
     '<manifest><activity name="A"/><activity name="B" main="true"/></manifest>', 'ok'),
    ('duplicate_name',
     '<manifest><activity name="A"/><activity name="A" main="true"/></manifest>',
     'error'),
    ('two_windows',
     '<manifest><activity name="A"><window/><window/></activity></manifest>', 'error'),
    ('activity_without_name', '<manifest><activity main="true"/></manifest>', 'error'),

    # ── 属性名 ──
    ('unknown_attr',
     '<manifest><activity><window widht="316"/></activity></manifest>', 'error'),
    ('snake_case_attr',
     '<manifest><activity><window min_width="316"/></activity></manifest>', 'error'),
    ('window_attr_on_activity',
     '<manifest><activity title="t"/></manifest>', 'error'),
    ('field_on_wrong_node',
     '<manifest><tray progId="P"/></manifest>', 'error'),
    ('extensions_as_attr',
     '<manifest><activity extensions="pynote"/></manifest>', 'error'),

    # ── 类型 ──
    ('int_not_number',
     '<manifest><activity><window width="abc"/></activity></manifest>', 'error'),
    ('int_float',
     '<manifest><activity><window width="31.6"/></activity></manifest>', 'error'),
    ('bool_not_bool',
     '<manifest><activity><window resizable="1"/></activity></manifest>', 'error'),
    ('bool_upper_ok',
     '<manifest><activity name="A"><window resizable="True"/></activity></manifest>',
     'ok'),

    # ── 语义 ──
    ('negative_width',
     '<manifest><activity><window width="-1"/></activity></manifest>', 'error'),
    ('min_gt_width',
     '<manifest><activity><window width="100" minWidth="200"/></activity></manifest>',
     'error'),
    ('min_gt_height',
     '<manifest><activity><window height="100" minHeight="200"/></activity></manifest>',
     'error'),
    ('empty_layout',
     '<manifest><activity><window layout=""/></activity></manifest>', 'error'),
    ('zero_sizes_ok',
     '<manifest><activity name="A"><window width="0" height="0" minWidth="0"/>'
     '</activity></manifest>',
     'ok'),
]

# 清单与脚本常量的冲突。脚本里写 WIDTH，清单里写 width —— 必须报错。
CONFLICT_SOURCE = 'WIDTH = 316\nx = 1\n'
CONFLICT_MANIFEST = {'width': 400}


def read_manifest(text):
    """把文本写进临时文件再走 load_manifest，保证测的是真实入口。"""
    directory = tempfile.mkdtemp(prefix='pyui_manifest_')
    path = os.path.join(directory, 'manifest.xml')
    with open(path, 'w', encoding='utf-8') as handle:
        handle.write(text)
    try:
        return pyuic.load_manifest(path)
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def run():
    failures = 0

    for name, text, expected in CASES:
        try:
            read_manifest(text)
            ok = True
            message = ''
        except pyuic.ManifestError as error:
            ok = False
            message = str(error)
        except Exception as error:  # 崩溃比判错更严重
            print('FAIL %-24s 抛出了意料之外的 %s: %s'
                  % (name, type(error).__name__, error))
            failures += 1
            continue

        want_ok = expected == 'ok'
        if ok != want_ok:
            if want_ok:
                print('FAIL %-24s 期望能读，却报错：%s' % (name, message))
            else:
                print('FAIL %-24s 期望报错，却接受了' % name)
            failures += 1
            continue

        # 报错必须有内容。空消息等于「构建失败但不说为什么」，比不检查还差。
        if not ok and not message.strip():
            print('FAIL %-24s 报错了但没有说明原因' % name)
            failures += 1

    # ── 语法错误必须带位置 ──
    #
    # 手写清单最常见的错误是标签不配对、属性少引号。没有行列号的话，
    # 用户只能对着整份文件猜，这正是「解析器和报错同等重要」的原因。
    for name, text in (
            ('syntax_unclosed', '<manifest>\n  <window width="1">\n</manifest>'),
            ('syntax_missing_quote', '<manifest>\n  <window width="1/>\n</manifest>'),
            ('syntax_bad_close', '<manifest>\n  <window></file>\n</manifest>')):
        try:
            read_manifest(text)
            print('FAIL %-24s 期望报语法错误，却接受了' % name)
            failures += 1
        except pyuic.ManifestError as error:
            if error.line is None or error.column is None:
                print('FAIL %-24s 语法错误没有带行列号：%s' % (name, error.message))
                failures += 1
        except Exception as error:
            print('FAIL %-24s 抛出了意料之外的 %s: %s'
                  % (name, type(error).__name__, error))
            failures += 1

    # ── 别名必须落到 AppSpec 的真实字段上 ──
    #
    # <activity description> 与 <tray tooltip> 在 XML 里省掉了节点名前缀，
    # 一旦别名表漏了一项，生成的代码会去写 spec.description / spec.tooltip
    # 这种不存在的字段——C++ 编译期才发现，报错位置离清单很远。
    data, _ = read_manifest(
        '<manifest><activity name="A" description="d">'
        '<extension>a</extension></activity><tray tooltip="t"/></manifest>')
    for attr, field in (('description', 'fileDescription'), ('tooltip', 'trayTooltip')):
        if data.get(field) is None:
            print('FAIL %-24s <%s> 没有落到 %s' % ('manifest_alias', attr, field))
            failures += 1

    # ── 冲突检测 ──
    #
    # 这一条必须走 Generator：冲突是在生成阶段发现的（它同时看得到清单
    # 与脚本的 AST）。期望报错，且报错位置指向脚本里那行。
    try:
        tree = ast.parse(CONFLICT_SOURCE)
        pyuic.Generator('ProbeScript', '<probe>',
                        manifest=CONFLICT_MANIFEST).generate(tree)
        print('FAIL %-24s 清单与脚本写同一字段，却没有报冲突' % 'manifest_conflict')
        failures += 1
    except pyuic.CompileError as error:
        if error.line != 1:
            print('FAIL %-24s 冲突报错指到了第 %d 行，应该是第 1 行'
                  % ('manifest_conflict', error.line))
            failures += 1
    except Exception as error:
        print('FAIL %-24s 抛出了意料之外的 %s: %s'
              % ('manifest_conflict', type(error).__name__, error))
        failures += 1

    # ── 无冲突时清单必须真的进 describe ──
    try:
        tree = ast.parse('x = 1\n')
        code = pyuic.Generator('ProbeScript', '<probe>',
                               manifest={'width': 400, 'title': 'P',
                                         'extensions': ['pynote']}).generate(tree)
        for needle in ('spec.width = 400;', 'spec.title = "P";',
                       'spec.extensions = {"pynote"};'):
            if needle not in code:
                print('FAIL %-24s 生成代码里缺少 %s' % ('manifest_emit', needle))
                failures += 1
    except Exception as error:
        print('FAIL %-24s 生成失败 %s: %s'
              % ('manifest_emit', type(error).__name__, error))
        failures += 1

    total = len(CASES) + 3 + 1 + 1 + 1
    if failures:
        print('manifest check: %d 个用例与期望不符（共 %d 个）' % (failures, total))
        return 1
    print('manifest check OK: %d 个用例' % total)
    return 0


if __name__ == '__main__':
    sys.exit(run())
