#!/usr/bin/env python3
# pyuic：把 Python 脚本编译成 C++
#
# ── 为什么前端用 ast ──
#
# 早先这条路是手写词法分析 + 递归下降解析器。它能跑，但那是「与 CPython
# 语法定义的第二份实现」：CPython 每加一个写法就要跟着改一次，而且报错
# 位置经常指错行（比如 `EXTENSIONS = [...]` 被当成类型错误）。
#
# 换成标准库 ast 之后，语法覆盖范围直接等于当前所用的 CPython，行号是
# CPython 自己算的，诊断信息也准确。我们只保留无法外包的那一半：
# AST → C++ 的代码生成。
#
# ── 为什么是转译而不是内嵌解释器 ──
#
#   1. 体积：CPython 解释器 + 标准库压缩后 4~8 MB，转译不占一个字节。
#   2. 速度：转译后是直接调用，没有字节码派发、名字查表、引用计数。
#   3. 部署：产物里没有 .py、没有 import 路径问题。
#
# 代价是「动态特性有限」（没有 exec/getattr/装饰器），但这是**构建期**
# 报错，带 file:line:col，而不是运行期某个分支悄悄不生效。
#
# ── 变量为什么是 py::Value ──
#
# 脚本里 `x = "0"` 之后可能是数字，Python 本来就允许同名换类型。生成 C++ 时
# 不做类型推断，统一用 py::Value 承载，编译期只剩一件事要保证：名字能解析。
#
# ── 用法 ──
#
#   python -m pyuic --out <生成目录> [--header <汇总头>] <脚本.py>...
#
# 每个脚本产出 <stem>.py.gen.cpp 与 <stem>.py.gen.h（工厂声明）。
import argparse
import ast
import json
import os
import sys
import xml.etree.ElementTree as ElementTree

# 清单读取与 pyuic 同目录。用显式 sys.path 而不是相对 import：
# 这个文件既是脚本（python pyuic.py）又是被 import 的模块（check_syntax.py）。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extensions  # noqa: E402


# ═══════════════════════════════════════════════════════════
# 诊断
# ═══════════════════════════════════════════════════════════

class CompileError(Exception):
    """构建期错误。带文件名/行/列，由 main 统一打印。"""

    def __init__(self, message, line=0, column=0):
        super().__init__(message)
        self.message = message
        self.line = line
        self.column = column


def node_error(message, node):
    raise CompileError(message, getattr(node, 'lineno', 0),
                       getattr(node, 'col_offset', 0) + 1)


def describe_statement(node):
    """语句节点 → 用户看得懂的中文名。

    直接打 type(node).__name__ 会得到「不支持 Try 语句」这种半英文的句子，
    用户搜不到也猜不出该改成什么。这张表只在报错路径上用到。

    TryStar / Match 是 3.10/3.11 才有的节点类型，而本工具要在 3.8 上也能跑，
    所以只能按「存在才登记」的方式建表——直接写 ast.TryStar 在旧版本上会
    在 import 阶段就 AttributeError。
    """
    names = [
        ('With', 'with'), ('AsyncWith', 'async with'), ('Try', 'try/except'),
        ('TryStar', 'try/except*'), ('AsyncFunctionDef', 'async def'),
        ('AsyncFor', 'async for'), ('Match', 'match/case'),
    ]
    for attribute, name in names:
        kind = getattr(ast, attribute, None)
        if kind is not None and isinstance(node, kind):
            return name
    return type(node).__name__


def describe_remedy(node):
    """给报错配一句「那该怎么办」。报错只说不行，用户还得再查一遍文档。"""
    for attribute in ('Try', 'TryStar'):
        kind = getattr(ast, attribute, None)
        if kind is not None and isinstance(node, kind):
            return '没有异常通道，失败请用返回值表示'
    for attribute in ('With', 'AsyncWith'):
        kind = getattr(ast, attribute, None)
        if kind is not None and isinstance(node, kind):
            return '没有上下文管理器，资源请在结束时显式收尾'
    return '请改写为普通语句'


# ═══════════════════════════════════════════════════════════
# 名字表
#
# 这三张表决定了「脚本里的一行」落到哪个 C++ 调用上。它们必须与
# include/pyui/py_script.h、include/pyui/py_value.h 同步——漏一个就是
# 编译期「没有这个函数」，所以清单在这里集中管理。
# ═══════════════════════════════════════════════════════════

# 模块顶层可以赋值的元数据常量：构建期读进 AppSpec，窗口要用它们建出来。
META_FIELDS = {
    'TITLE': ('title', 'string'),
    'LAYOUT': ('layout', 'string'),
    'THEME': ('theme', 'string'),
    'PROG_ID': ('progId', 'string'),
    'FILE_DESCRIPTION': ('fileDescription', 'string'),
    'TRAY_TOOLTIP': ('trayTooltip', 'string'),
    'WIDTH': ('width', 'int'),
    'HEIGHT': ('height', 'int'),
    'MIN_WIDTH': ('minWidth', 'int'),
    'MIN_HEIGHT': ('minHeight', 'int'),
    'FRAME_INTERVAL': ('frameInterval', 'int'),
    'CUSTOM_TITLE_BAR': ('customTitleBar', 'bool'),
    'RESIZABLE': ('resizable', 'bool'),
    'EXTENSIONS': ('extensions', 'string_list'),
}

# 自由函数：脚本里的写法 → py:: 命名空间下的函数名。
# 值是 (C++ 名, 参数个数下限, 上限)。None 表示不限。
FREE_FUNCTIONS = {
    # 内建
    'int': ('toInteger', 1, 1),
    'float': ('toNumber', 1, 1),
    'str': ('toText', 1, 1),
    'bool': ('boolOf', 1, 1),
    'len': ('length', 1, 1),
    'abs': ('absolute', 1, 1),
    'round': ('roundValue', 1, 2),
    'min': ('minimum', 2, 2),
    'max': ('maximum', 2, 2),
    'sum': ('sumOf', 1, 1),
    'list': ('clone', 1, 1),
    'dict': ('dictFrom', 1, 2),
    'range': (None, 1, 3),   # 只允许出现在 for 里
    'print': ('log', 1, 1),
    'char_from_code': ('charFromCode', 1, 1),
    # 脚本名沿用 PyUI 自己的 `parse_number`（这不是 CPython 的内建名），
    # 实现直接复用 AiPy 的 parseFloat：两者语义相同——整串必须都是数字，
    # 失败返回 None 而不是 12abc → 12。
    'parse_number': ('parseFloat', 1, 1),
    # 文件与路径不在这里：它们属于扩展能力，由 ext/fs 提供，
    # 脚本里写 `import fs` 后走 extension_call。核心不含任何 IO——
    # 一个不读文件的程序不该把 fopen 和路径处理带进 exe。
    # 控件
    'set_text': ('setText', 2, 2),
    'text': ('text', 1, 1),
    'set_label': ('setLabel', 2, 2),
    'label': ('label', 1, 1),
    'set_placeholder': ('setPlaceholder', 2, 2),
    'set_progress': ('setProgress', 2, 2),
    'progress': ('progress', 1, 1),
    'set_indeterminate': ('setIndeterminate', 2, 2),
    'set_checked': ('setChecked', 2, 2),
    'checked': ('checked', 1, 1),
    'set_value': ('setValue', 2, 2),
    'value': ('value', 1, 1),
    'set_visible': ('setVisible', 2, 2),
    'set_enabled': ('setEnabled', 2, 2),
    'set_color': ('setColor', 2, 2),
    'set_text_color': ('setTextColor', 2, 2),
    'focus': ('focus', 1, 1),
    # 字符串
    'upper': ('upper', 1, 1),
    'lower': ('lower', 1, 1),
    'strip': ('strip', 1, 1),
    'replace': ('replace', 3, 3),
    'split': ('split', 2, 2),
    'join': ('join', 2, 2),
    'starts_with': ('startsWith', 2, 2),
    'ends_with': ('endsWith', 2, 2),
    'find_in': ('findIn', 2, 2),
    'count_in': ('countIn', 2, 2),
    'format_text': ('formatText', 2, 2),
    # 菜单
    'menu_item': ('makeMenuItem', 2, 2),
    'menu_separator': ('makeMenuSeparator', 0, 0),
    # 事件绑定
    'on_click': ('onClick', 2, 2),
    'on_double_click': ('onDoubleClick', 2, 2),
    'on_change': ('onChange', 2, 2),
    'on_commit': ('onCommit', 2, 2),
    'on_submit': ('onSubmit', 2, 2),
    'on_context_menu': ('onContextMenu', 2, 2),
    'log': ('log', 1, 1),
    'is_none': ('isNone', 1, 1),
}

# 事件绑定：第二个参数必须是函数名，不是值。值 = 回调接受几个 py::Value。
EVENT_BINDINGS = {
    'on_click': ('onClick', 0),
    'on_double_click': ('onDoubleClick', 0),
    'on_context_menu': ('onContextMenu', 0),
    'on_change': ('onChange', 1),
    'on_commit': ('onCommit', 1),
    'on_submit': ('onSubmit', 1),
}

# 方法调用：<接收者>.<方法>(...) → py::<函数>(<接收者>, ...)
# 列表/字典方法就地修改，所以接收者必须是可寻址的东西（变量或下标）。
METHODS = {
    # 列表
    'append': ('append', 1, 1),
    'extend': ('extend', 1, 1),
    'insert': ('insertAt', 2, 2),
    'pop': ('popAt', 1, 1),
    'clear': ('clearContainer', 0, 0),
    'sort': ('sortList', 0, 0),
    'reverse': ('reverseList', 0, 0),
    # 字典
    'get': ('dictGetOr', 2, 2),
    'keys': ('dictKeysOf', 0, 0),
    'values': ('dictValuesOf', 0, 0),
    'items': ('dictItemsOf', 0, 0),
    'update': ('dictUpdate', 1, 1),
    # 字符串
    'upper': ('upper', 0, 0),
    'lower': ('lower', 0, 0),
    'strip': ('strip', 0, 0),
    'replace': ('replace', 2, 2),
    'split': ('split', 1, 1),
    'join': ('join', 1, 1),
    'startswith': ('startsWith', 1, 1),
    'endswith': ('endsWith', 1, 1),
    'find': ('findIn', 1, 1),
    'count': ('countIn', 1, 1),
    'format': ('formatText', 1, 1),
    # 控件（方法形式与自由函数形式等价，见 py_script.h 的说明）
    'set_text': ('setText', 1, 1),
    'set_label': ('setLabel', 1, 1),
    'set_placeholder': ('setPlaceholder', 1, 1),
    'set_progress': ('setProgress', 1, 1),
    'set_checked': ('setChecked', 1, 1),
    'set_value': ('setValue', 1, 1),
    'set_visible': ('setVisible', 1, 1),
    'set_enabled': ('setEnabled', 1, 1),
    'focus': ('focus', 0, 0),
}

# 需要 this-> 的成员函数：它们依赖窗口/根容器，不是普通自由函数。
SCRIPT_MEMBERS = {
    'find': ('find', 1, 1),
    'show_tray': ('trayShow', 1, 1),
    'hide_tray': ('trayHide', 0, 0),
    'set_tray_tooltip': ('traySetTooltip', 1, 1),
    'popup_menu': ('menuPopup', 1, 1),
    'register_association': ('associateFile', 3, 3),
    'association_registered': ('associationExists', 1, 1),
    'quit': ('quit', 0, 1),
    'refresh': ('refresh', 0, 0),
    'set_theme': ('setTheme', 1, 1),
    'theme_name': ('themeName', 0, 0),
    'set_title': ('setTitle', 1, 1),
}

# 生命周期钩子：脚本里的 def 名字 → Script 虚函数签名。
# 生成的代码把这些 def 变成 py::Value 普通函数，再写一层转发。
LIFECYCLE_HOOKS = {
    'build', 'on_frame', 'on_resize', 'on_key', 'on_char', 'on_close',
    'on_file_drop', 'open_file', 'on_tray_activate', 'tray_menu', 'on_tray_command',
}

# 保留字（不能给变量起名）
RESERVED = set(dir(__builtins__) if not isinstance(__builtins__, dict) else __builtins__)


# ═══════════════════════════════════════════════════════════
# C++ 字面量
# ═══════════════════════════════════════════════════════════

def cpp_string(value):
    """把 Python 字符串转成 C++ 字面量。生成的源文件按 UTF-8 编译。"""
    out = ['"']
    for ch in value:
        if ch == '"':
            out.append('\\"')
        elif ch == '\\':
            out.append('\\\\')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\r':
            out.append('\\r')
        elif ch == '\t':
            out.append('\\t')
        elif ord(ch) < 0x20:
            out.append('\\x%02X' % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)


def cpp_double(value):
    """浮点字面量。整数值也要带 .0，否则 1/3 会走整数除法。"""
    text = repr(float(value))
    return text


# ═══════════════════════════════════════════════════════════
# 应用清单
# ═══════════════════════════════════════════════════════════

# 清单文件名：固定叫 manifest.xml，与脚本同目录。
#
#   examples/calculator/manifest.xml
#   examples/calculator/calculator.py
#
# 为什么是固定名字而不是 calculator.json：清单描述的是**整个应用**，不是
# 某个脚本的附件。一个 exe 只可能有一份窗口声明，用固定名字之后「这个程序
# 长什么样」永远在同一个地方，不需要先看脚本叫什么再去找同名文件。
# 这一点与 AndroidManifest.xml 一致：名字固定，位置固定。
#
# 为什么是 XML 而不是 JSON：同一套应用里清单与布局都是 XML，错误报法、
# 注释写法、属性命名只需要解释一次。清单有层级（window / files / tray），
# 扁平 JSON 反而要靠键名前缀来表达归属。
#
# 结构照 AndroidManifest 的分工：
#
#   <manifest name="calculator">
#     <window title="计算器" width="316" height="512" .../>
#     <files progId="..." description="..."><extension>pynote</extension></files>
#     <tray tooltip="..."/>
#   </manifest>

MANIFEST_FILENAME = 'manifest.xml'

# 每个子节点允许的属性与取值类型。集中在一张表里，是为了让「支持哪些属性」
# 成为一处可读的说明，而不是散在十几处 if 里。
MANIFEST_ROOT_ATTRS = {
    'name': 'string',
}
# <activity>：入口单位。名字就是脚本里那个类名，其余属性描述「这个界面」
# 与「它关联的文件」——与 Android 把 intent-filter 挂在 activity 上同一个
# 理由：文件关联最终是「交给哪个界面打开」，不是整个程序的属性。
MANIFEST_ACTIVITY_ATTRS = {
    'name': 'string',
    'main': 'bool',
    'progId': 'string',
    'description': 'string',
}
MANIFEST_WINDOW_ATTRS = {
    'title': 'string',
    'layout': 'string',
    'width': 'int',
    'height': 'int',
    'minWidth': 'int',
    'minHeight': 'int',
    'theme': 'string',
    'customTitleBar': 'bool',
    'resizable': 'bool',
    'frameInterval': 'int',
}
MANIFEST_TRAY_ATTRS = {
    'tooltip': 'string',
}

# 清单属性 → AppSpec 字段。与 META_FIELDS 的第二个元素是同一套名字，
# 因为两者最终写的都是 AppSpec。
#
# 需要别名的只有这两个：XML 里节点已经表达了归属（<activity> / <tray>），
# 属性名就不该再重复一遍节点名——`<tray tooltip="...">` 比
# `trayTooltip="..."` 好写也好读。但 AppSpec 只有一层字段，所以这里转回全名。
_MANIFEST_FIELD_ALIAS = {
    'description': 'fileDescription',
    'tooltip': 'trayTooltip',
}


class ManifestError(Exception):
    def __init__(self, message, line=None, column=None):
        Exception.__init__(self, message)
        self.message = message
        self.line = line
        self.column = column


def _manifest_line_of(text, offset):
    """字符偏移 → (行, 列)，都是 1 开始。"""
    line = text.count('\n', 0, offset) + 1
    last_newline = text.rfind('\n', 0, offset)
    column = offset - last_newline
    return line, column


def _convert_manifest_value(kind, raw, attr, where):
    """把 XML 属性文本转成 Python 值，失败时抛 ManifestError。"""
    if kind == 'string':
        return raw
    if kind == 'int':
        try:
            return int(raw, 10)
        except ValueError:
            raise ManifestError('%s="%s" 应该是整数' % (attr, raw), where[0], where[1])
    if kind == 'bool':
        lowered = raw.strip().lower()
        if lowered == 'true':
            return True
        if lowered == 'false':
            return False
        raise ManifestError('%s="%s" 应该是 true 或 false' % (attr, raw), where[0], where[1])
    raise ManifestError('内部错误：未知的属性类型 %s' % kind)


def _merge_attrs(element, allowed, result):
    """把 `<element>` 的属性校验后合并进 result。

    属性名与 AppSpec 字段的差异只存在于别名表里，其余一律同名——这是
    「清单写什么、生成代码就写什么」的前提，也是报错能直接指出键名的原因。
    """
    for attr, raw_value in element.attrib.items():
        if attr not in allowed:
            raise ManifestError(
                '<%s> 没有属性 %s；可用：%s'
                % (element.tag, attr, '、'.join(sorted(allowed))))
        field = _MANIFEST_FIELD_ALIAS.get(attr, attr)
        try:
            result[field] = _convert_manifest_value(allowed[attr], raw_value, attr, (1, 1))
        except ManifestError as error:
            # 补上位置：ElementTree 不保留属性的精确位置，但至少能指出
            # 是哪个节点、哪个属性。清单通常只有十几行，这就够定位了。
            raise ManifestError('在 <%s> 上：%s' % (element.tag, error.message),
                                error.line, error.column)


def load_manifest(path):
    """读 manifest.xml 并校验。返回 (数据 dict, 警告列表)。

    文件不存在是合法的：老项目没有清单，全部走脚本常量。返回空 dict。
    """
    if not os.path.isfile(path):
        return {}, []

    try:
        with open(path, 'r', encoding='utf-8-sig') as handle:
            raw = handle.read()
    except OSError as error:
        raise ManifestError('打不开清单：%s' % error)

    # 用 ElementTree 而不是手写解析器：清单结构与布局同源，XML 解析这一层
    # 标准库已经做得比任何手写实现都可靠（实体、CDATA、命名空间、BOM）。
    # pyuic 本身就跑在 CPython 上，构建期多链一个模块没有任何代价。
    try:
        root = ElementTree.fromstring(raw)
    except ElementTree.ParseError as error:
        # ET 的异常自带 position=(行, 列)
        line, column = (error.position if getattr(error, 'position', None) else (1, 1))
        raise ManifestError('清单不是合法 XML：%s' % error, line, column + 1)

    if root.tag != 'manifest':
        raise ManifestError('清单根节点必须是 <manifest>，实际是 <%s>' % root.tag)

    # 文本节点一律不允许有非空白内容。清单全靠属性，出现裸文本几乎一定是
    # 手写时把属性写到了标签外面（`<window>width="316"</window>`），或者把
    # 注释写成了文本。ElementTree 默认会安静地把它塞进 .text，不查就丢了。
    def check_text(elem, label):
        if elem.text is not None and elem.text.strip():
            raise ManifestError('<%s> 里不允许有文本 "%s"；清单只用属性'
                                % (label, elem.text.strip()[:24]))

    check_text(root, 'manifest')
    for child in root:
        check_text(child, child.tag)
        if child.tail is not None and child.tail.strip():
            raise ManifestError('<%s> 之后不允许有文本 "%s"'
                                % (child.tag, child.tail.strip()[:24]))

    result = {}
    warnings = []

    # name 是根节点自己的属性（应用标识），其余都在子节点上。
    for attr in list(root.attrib):
        if attr not in MANIFEST_ROOT_ATTRS:
            raise ManifestError('<manifest> 没有属性 %s；可用：name' % attr)
    raw_name = root.get('name')
    if raw_name is not None:
        result['name'] = raw_name

    # <activity> 可以有多个，但必须恰好有一个 main="true"——那是「双击 exe
    # 之后先出现哪个界面」。与 Android 一样：声明可以多，入口只能一个。
    # 只有唯一一个 activity 时 main 可省略，省掉最常见的冗余。
    activities = []
    for child in root:
        if child.tag == 'tray':
            _merge_attrs(child, MANIFEST_TRAY_ATTRS, result)
            continue
        if child.tag != 'activity':
            raise ManifestError(
                '<manifest> 里不认识 <%s>；可用：<activity>、<tray>' % child.tag)

        activity = {}
        _merge_attrs(child, MANIFEST_ACTIVITY_ATTRS, activity)
        seen_window = False
        for item in child:
            if item.tag == 'window':
                if seen_window:
                    raise ManifestError('一个 <activity> 只能有一个 <window>')
                seen_window = True
                _merge_attrs(item, MANIFEST_WINDOW_ATTRS, activity)
            elif item.tag == 'extension':
                text = (item.text or '').strip()
                if not text:
                    raise ManifestError('<extension> 不能为空')
                activity.setdefault('extensions', []).append(text)
            else:
                raise ManifestError(
                    '<activity> 里不认识 <%s>；可用：<window>、<extension>' % item.tag)

        name = activity.get('name')
        if not name:
            raise ManifestError('<activity> 必须有 name：它就是脚本里那个类名')
        if any(other.get('name') == name for other in activities):
            raise ManifestError('有两个 <activity> 都叫 "%s"' % name)
        activities.append(activity)

    entry = None
    if activities:
        mains = [a for a in activities if a.get('main')]
        if len(mains) > 1:
            raise ManifestError('只能有一个 <activity main="true">（当前 %d 个）'
                                '：双击 exe 只能先进一个界面'
                                % len(mains))
        if mains:
            entry = mains[0]
        elif len(activities) == 1:
            entry = activities[0]
        else:
            raise ManifestError('有 %d 个 <activity>，必须给其中一个加 main="true"'
                                ' 指明入口' % len(activities))

        # 入口 activity 的字段提到顶层：AppSpec 是扁平的，其余脚本读的
        # 永远是自己那个界面的窗口描述。
        for key, value in entry.items():
            if key in ('name', 'main'):
                continue
            result[key] = value
        result['entry'] = entry['name']

    # 语义检查。这些放在构建期而不是等运行期：负的宽度会让布局算出负数，
    # 症状是窗口里什么都不显示，离原因非常远。
    width = result.get('width', 0)
    height = result.get('height', 0)
    min_width = result.get('minWidth', 0)
    min_height = result.get('minHeight', 0)
    interval = result.get('frameInterval', 0)
    for key, value in (('width', width), ('height', height),
                       ('minWidth', min_width), ('minHeight', min_height),
                       ('frameInterval', interval)):
        if value < 0:
            raise ManifestError('%s 不能是负数（当前 %d）' % (key, value))
    if width > 0 and min_width > width:
        raise ManifestError('minWidth(%d) 不能大于 width(%d)，窗口一创建就要被撑大'
                            % (min_width, width))
    if height > 0 and min_height > height:
        raise ManifestError('minHeight(%d) 不能大于 height(%d)'
                            % (min_height, height))

    if 'layout' in result and not result['layout']:
        raise ManifestError('layout 不能是空字符串；不想要布局就删掉这个属性')

    return result, warnings

class Generator:
    """把一个模块的 AST 转成 C++ 类。"""

    def __init__(self, class_name, filename, extensions=None, manifest=None):
        self.class_name = class_name
        self.filename = filename
        # 清单（应用级别的窗口描述）。脚本里显式写的元数据常量优先于它：
        # 脚本是逻辑，常量是「这个界面自己的选择」，更具体。
        self.manifest = manifest or {}
        # 模块级赋值过的名字 → 成员变量
        self.members = []
        self.member_set = set()
        # def 名 → ast.FunctionDef
        self.functions = {}
        # 元数据常量名，它们不进成员表
        self.meta_names = set()
        self.meta_lines = []
        # 元数据常量名 → 那条赋值的 AST 节点。清单与脚本冲突时要能指向
        # 脚本里那一行，否则使用者只能自己通篇找。
        self.meta_assign_node = {}
        # 当前作用域已声明的局部变量（按嵌套层数）
        self.locals = []
        # 当前函数里 `global x` 声明过的名字：它们指向成员而不是局部量。
        # 必须逐函数维护——同名的名字在一个函数里是全局、在另一个里是局部，
        # 这正是 Python 的规则。
        self.scope_globals = set()
        self.scope_locals = set()
        self.tree_body = []

        # ── Activity ──
        #
        # 脚本的写法对齐 Android：一个入口界面是一个继承 Activity 的类，
        # onCreate 里做绑定。生成出来仍然只有一个 C++ 类（一个脚本一个
        # 类），所以这里的 activity_node 只是「方法从哪儿来」的标记：
        # 类里的 def 去掉 self 之后与模块级 def 完全等价。
        self.activity_node = None
        self.activity_name = None
        # 类体里 `entry = "0"` 这种字段默认值：(成员名, 已生成的赋值语句)
        self.field_defaults = []
        # 哪些函数节点是 Activity 方法（带 self 参数）。params_of 据此去掉
        # 第一个参数——同一个 ast.FunctionDef 只可能属于其中一边。
        self.method_nodes = set()

        # ── 扩展 ──
        #
        # 定位：核心只提供最基础的 Python 语义，其余能力全部由扩展提供，
        # import 是唯一入口。所以这里要跟踪的是「这个脚本用了哪些模块」
        # 「每个模块的哪个函数对应哪个 C++ 符号」。
        self.extension_index = extensions
        # 本脚本 import 过的模块名（有序，生成 include 时保持稳定）
        self.used_modules = []
        self.used_module_set = set()
        # 本脚本用到过的 (模块, python 函数名) → 查表结果
        self.extension_calls = {}
        # 每个扩展函数的调用位置，写进 used.json 供报错与体积归因定位。
        # 结构：模块名 → {python 函数名 → [(文件, 行, 列)]}
        self.extension_sites = {}
        # 成员/自由函数之外的名字：`import fs` 之后 fs 本身也是个名字。
        # 它不能当变量用，否则 `fs = 1` 会让后面的 fs.read_file 变得没法解释。
        self.module_names = set()
        # `from fs import read_file` 导入进来的裸名字：python 名 → 模块名。
        # 它让 read_file(...) 能解析到这个函数来自哪个模块。
        self.imported_from = {}

    # ── 顶层 ──

    def generate(self, tree):
        self.collect(tree)
        self.check_activity_matches_manifest()
        self.check_manifest_conflicts()
        out = []
        out.append('// 由 pyuic 从 %s 生成，请勿手工修改' % os.path.basename(self.filename))
        out.append('// 前端是 CPython 的 ast，因此语法覆盖范围等于 CPython 本身。')
        out.append('')
        out.append('#include "pyui/py_script.h"')
        out.append('#include "pyui/py_value.h"')
        # 扩展的头：顺序按 used_modules（import 出现顺序），结果稳定可复现。
        # 只有真正 import 过的模块才会被 include——「用了才带进来」从这里开始。
        headers = []
        for module_name in self.used_modules:
            extension = self.extension_index.find(module_name)
            if extension is None or not extension.header:
                continue
            if extension.header not in headers:
                headers.append(extension.header)
        for header in headers:
            out.append('#include "%s"' % header)
        out.append('')
        out.append('#include <cstdint>')
        out.append('#include <string>')
        out.append('#include <vector>')
        out.append('')
        out.append('namespace pyui {')
        out.append('namespace py {')
        out.append('namespace generated {')
        out.append('')
        self.emit_class(out)
        out.append('')
        self.emit_bodies(out)
        out.append('}  // namespace generated')
        out.append('')
        # 工厂必须落在 pyui::py 里：main.cpp 通过 <stem>.py.gen.h 看到的
        # 声明就在那个命名空间，定义放在外面就是两个不同的符号——
        # 编译能过，链接报「找不到 createXxxScript」。
        out.append('Script* create%s() { return new generated::%s(); }' %
                   (self.class_name, self.class_name))
        out.append('')
        out.append('}  // namespace py')
        out.append('}  // namespace pyui')
        out.append('')
        return '\n'.join(out)

    # ── 各函数体 ──

    def emit_bodies(self, out):
        # describe：清单 + 顶层元数据常量 → AppSpec。
        #
        # 每个字段**只能有一个来源**：清单与脚本同时写同一个字段会在
        # check_manifest_conflicts 里报错。合并两处来源看着更宽容，
        # 但代价是「改了一处没生效」这类问题——配置项最容易在这里失控。
        out.append('void %s::describe(py::AppSpec& spec) {' % self.class_name)
        manifest_lines = self.manifest_lines()
        lines = manifest_lines + self.meta_lines
        if lines:
            for line in lines:
                out.append('  ' + line)
        else:
            out.append('  (void)spec;')
        out.append('}')
        out.append('')

        self.emit_init_and_build(out)

    def manifest_lines(self):
        """把清单字段转成 spec 赋值语句。

        键名与 META_FIELDS 是同一套（camelCase），因为两者最终写的是同一个
        AppSpec。清单少写某个键就保持 AppSpec 的默认值，与「不写脚本常量」
        完全等价。
        """
        lines = []
        # 顺序固定，保证同样的清单每次生成同样的代码（可重现构建）。
        for key in ('title', 'layout', 'width', 'height', 'minWidth', 'minHeight',
                    'theme', 'customTitleBar', 'resizable', 'frameInterval',
                    'progId', 'fileDescription', 'trayTooltip'):
            if key not in self.manifest:
                continue
            value = self.manifest[key]
            if isinstance(value, bool):
                lines.append('spec.%s = %s;' % (key, 'true' if value else 'false'))
            elif isinstance(value, int):
                lines.append('spec.%s = %d;' % (key, value))
            else:
                lines.append('spec.%s = %s;' % (key, cpp_string(value)))
        if 'extensions' in self.manifest:
            items = self.manifest['extensions']
            if items:
                literal = ', '.join(cpp_string(item) for item in items)
                lines.append('spec.extensions = {%s};' % literal)
        return lines

    def check_activity_matches_manifest(self):
        """清单里的 <activity name> 必须与脚本里的类名一致。

        两者对不上时，症状是「清单写对了、界面却不按清单长」，因为生成代码
        读的是脚本这一侧。清单是应用的声明处，脚本是它的实现——名字是唯一
        把两者绑起来的键，必须在构建期对上。

        没有 Activity 类的老脚本不检查：它们走「模块级 def build()」的老写法，
        清单里的 name 只是给构建系统看的标识。
        """
        declared = self.manifest.get('entry')
        if declared is None or self.activity_name is None:
            return
        if declared != self.activity_name:
            node_error(
                '清单里的 <activity name="%s"> 与脚本里的 class %s(Activity) 不一致。'
                '名字是把声明和实现绑起来的唯一键，必须写成同一个'
                % (declared, self.activity_name), self.activity_node)

    def check_manifest_conflicts(self):
        """清单与脚本常量写同一个字段时报错。

        为什么不「脚本覆盖清单」：两处都能写同一件事，迟早会有人只改了
        其中一处然后对着没生效的窗口排查半天。合并成一份之后，每个字段
        只有唯一来源——这是配置项最容易失控的地方。

        允许迁移期：脚本里的常量可以先留着，但一旦清单里有同一个键，
        就必须二选一。
        """
        for name in self.meta_names:
            field, _ = META_FIELDS[name]
            if field in self.manifest:
                node_error(
                    '"%s" 与清单里的 "%s" 重复。窗口描述只能有一个来源：'
                    '删掉脚本里的 %s = ...，或者删掉清单里的 "%s" 键'
                    % (name, field, name, field),
                    self.meta_assign_node.get(name))

    def emit_init_and_build(self, out):
        """initState / build / 普通函数。从 emit_bodies 拆出来，
        让 describe（元数据）与模块体（逻辑）各自独立。"""
        # initState：模块级语句（变量初值）。
        # 与 build() 分开的理由见 py_script.h：模块体在 import 时就执行完，
        # 而 build() 是「界面已挂好」之后的钩子。
        out.append('void %s::initState() {' % self.class_name)
        # 顶层作用域就是「全局作用域」：这里的名字要么是成员，要么是
        # 顶层临时量。先扫出被赋值的名字，才能让「先用后赋值」不误报。
        top_locals = set()
        for node in self.tree_body:
            if isinstance(node, (ast.FunctionDef, ast.ClassDef)):
                continue
            if self.is_meta_assign(node):
                continue
            self.collect_assigned(node, top_locals)
        top_declared = set()
        for node in self.tree_body:
            if isinstance(node, ast.Global):
                top_declared.update(node.names)
        top_locals -= top_declared
        top_locals = set(n for n in top_locals if n not in self.member_set)

        self.locals = [top_locals]
        top_level = []
        for node in self.tree_body:
            if isinstance(node, (ast.FunctionDef, ast.ClassDef)):
                continue
            # import 是构建期指令：它已经变成了 include 与链接，运行期没有语句
            if isinstance(node, (ast.Import, ast.ImportFrom)):
                continue
            # 元数据常量已经在 describe() 里消费，不再当变量生成
            if self.is_meta_assign(node):
                continue
            top_level.extend(self.statement(node, 2))
        self.locals = []
        # 顶层临时量同样在函数开头声明（C++ 的块作用域与 Python 的
        # 模块作用域不同，这里只有一层，但保持与函数内一致的写法）
        for local in sorted(top_locals):
            out.append('  py::Value %s;' % local)
        if top_level:
            out.extend(top_level)
        out.append('}')
        out.append('')

        # build() 是宿主唯一调用的入口（Script::build 虚函数）。它按固定顺序
        # 做三件事：
        #
        #   1. Activity 的字段初值（等价于 __init__ 里的 self.x = ...）
        #   2. 模块级的 def build()——老写法，保留
        #   3. Activity 的 onCreate()——新写法，界面已挂好之后绑定事件
        #
        # 都走 emit_function：它们与普通函数遵守同一套作用域规则，单独写一份
        # 会让「忘了 self.」这类错误只有它们不报。
        out.append('void %s::build() {' % self.class_name)
        # 为什么字段初值不放构造函数：构造函数在窗口存在之前就跑，那时任何
        # 依赖 find() 的初值都还没有意义。这里与 Python 的「实例先建好、
        # onCreate 才拿到界面」是同一个时序。
        for name, literal in self.field_defaults:
            out.append('  %s_ = %s;' % (name, literal))
        out.append('  runBuildHook();')
        out.append('  runOnCreate();')
        out.append('}')
        out.append('')

        if 'build' in self.functions:
            func = self.functions['build']
            if self.params_of(func):
                node_error('build() 不接受参数', func)
            self.emit_function(out, 'runBuildHook', func, return_type='void')
        else:
            out.append('void %s::runBuildHook() {}' % self.class_name)
        out.append('')

        if 'onCreate' in self.functions:
            func = self.functions['onCreate']
            if self.params_of(func):
                node_error('onCreate(self) 不接受参数', func)
            self.emit_function(out, 'runOnCreate', func, return_type='void')
        else:
            out.append('void %s::runOnCreate() {}' % self.class_name)
        out.append('')

        # 普通函数
        for name in self.functions:
            if name in ('build', 'onCreate'):
                continue
            out.append('')
            self.emit_function(out, name, self.functions[name])
            out.append('')

        # 生命周期转发：脚本里的 def on_key/on_char/… 是普通函数，
        # 宿主调的是 Script 的虚函数，所以中间要一层签名转换。
        self.emit_hooks(out)

    def emit_hooks(self, out):
        if 'on_frame' in self.functions:
            out.append('void %s::onFrame() { (void)on_frame(); }' % self.class_name)
            out.append('')
        if 'on_resize' in self.functions:
            out.append('void %s::onResize(int argWidth, int argHeight) {' % self.class_name)
            out.append('  (void)on_resize(py::Value(argWidth), py::Value(argHeight));')
            out.append('}')
            out.append('')
        if 'on_key' in self.functions:
            out.append('bool %s::onKeyDown(const Key& argKey, const Modifiers& argMods) {'
                       % self.class_name)
            out.append('  return on_key(py::Value(py::keyName(argKey)), '
                       'py::modifiersValue(argMods)).toBool();')
            out.append('}')
            out.append('')
        if 'on_char' in self.functions:
            out.append('bool %s::onChar(std::uint32_t argCodepoint) {' % self.class_name)
            out.append('  return on_char(py::Value(static_cast<long long>(argCodepoint))).toBool();')
            out.append('}')
            out.append('')
        if 'on_close' in self.functions:
            out.append('bool %s::onCloseRequest() { return on_close().toBool(); }'
                       % self.class_name)
            out.append('')
        if 'on_file_drop' in self.functions:
            out.append('void %s::onFileDrop(const std::vector<std::string>& argPaths) {'
                       % self.class_name)
            out.append('  (void)on_file_drop(py::Value::list(py::Value::List('
                       'argPaths.begin(), argPaths.end())));')
            out.append('}')
            out.append('')
        if 'open_file' in self.functions:
            out.append('void %s::openFile(const std::string& argPath) {' % self.class_name)
            out.append('  (void)open_file(py::Value(argPath));')
            out.append('}')
            out.append('')
        if 'on_tray_activate' in self.functions:
            out.append('void %s::onTrayActivate() { (void)on_tray_activate(); }'
                       % self.class_name)
            out.append('')
        if 'tray_menu' in self.functions:
            out.append('std::vector<MenuItem> %s::trayMenu() {' % self.class_name)
            out.append('  return py::menuItemsFrom(tray_menu());')
            out.append('}')
            out.append('')
        if 'on_tray_command' in self.functions:
            out.append('void %s::onTrayCommand(const std::string& argId) {' % self.class_name)
            out.append('  (void)on_tray_command(py::Value(argId));')
            out.append('}')
            out.append('')

    def collect(self, tree):
        """先扫一遍：分清成员变量、函数、元数据。"""
        self.tree_body = tree.body
        for node in tree.body:
            if isinstance(node, ast.Assign):
                self.collect_assign(node)
            elif isinstance(node, ast.AnnAssign) and node.value is not None:
                self.collect_assign(node)
            elif isinstance(node, ast.AugAssign):
                self.collect_augassign(node)
            elif isinstance(node, ast.FunctionDef):
                if node.name in self.functions:
                    node_error('函数 "%s" 定义了两次' % node.name, node)
                # 装饰器在 CPython 里是「f = deco(f)」，会替换掉函数对象。
                # 生成代码直接调用原函数，等于悄悄丢掉装饰器——@staticmethod
                # 之类的写法会「编译通过但行为不同」。构建期报错更安全。
                if node.decorator_list:
                    node_error('不支持装饰器：脚本里没有一等函数，装饰器无从生效',
                               node.decorator_list[0])
                self.functions[node.name] = node
            elif isinstance(node, (ast.Import, ast.ImportFrom)):
                self.collect_import(node)
            elif isinstance(node, ast.ClassDef):
                self.collect_activity(node)
            elif isinstance(node, (ast.With, ast.Try, ast.AsyncFunctionDef)):
                node_error('不支持 %s 语句' % describe_statement(node), node)
            elif isinstance(node, (ast.If, ast.For, ast.While, ast.Expr, ast.Return,
                                   ast.Pass, ast.Break, ast.Continue, ast.Raise,
                                   ast.Assert, ast.Delete, ast.Global, ast.Nonlocal)):
                pass  # 顶层允许，但 global 在顶层没有意义，后面会忽略
            else:
                node_error('不支持顶层语句 %s' % type(node).__name__, node)

    def collect_activity(self, node):
        """收集 `class Xxx(Activity)`。

        为什么只允许一个 Activity、且必须显式继承 Activity：产物是一个进程
        一个入口界面（见清单里只能有一个 <activity>）。允许任意 class 会让
        「这个类到底做什么」没有答案——Python 子集里没有对象模型，类只是
        「一组方法与字段的容器」，那正是 Activity 的含义。

        类里的 def 与模块级 def 编译成同一个 C++ 类的成员函数，`self`
        在生成时被去掉：一个脚本只有一个实例，`self` 与 `this` 是同一个东西，
        保留它只会让每个方法多一个没人用的参数。
        """
        if self.activity_node is not None:
            node_error('只能有一个 Activity 类；第二个用 %s' % node.name, node)
        if node.decorator_list:
            node_error('不支持装饰器', node.decorator_list[0])
        if len(node.bases) != 1:
            node_error('Activity 必须写成一个基类：class %s(Activity)' % node.name, node)
        base = node.bases[0]
        if not (isinstance(base, ast.Name) and base.id == 'Activity'):
            node_error('Activity 的基类必须是 Activity', base)
        if node.keywords:
            node_error('不支持带关键字的基类', node)

        self.activity_node = node
        self.activity_name = node.name

        # 先扫一遍 self.xxx 的赋值，才能在生成表达式时把 self.xxx 解析成成员。
        # 类体里的 `entry = "0"` 与 __init__ 里的 `self.entry = "0"` 是同一
        # 件事：都是这个实例的字段初值。
        for statement in node.body:
            if isinstance(statement, ast.FunctionDef):
                self.scan_self_members(statement)

        for statement in node.body:
            if isinstance(statement, ast.FunctionDef):
                if statement.name in self.functions:
                    node_error('函数 "%s" 定义了两次' % statement.name, statement)
                self.declare_method(statement)
            elif isinstance(statement, (ast.Assign, ast.AnnAssign)):
                if isinstance(statement, ast.AnnAssign) and statement.value is None:
                    continue  # 只有类型标注：Python 里不产生值
                targets = statement.targets if isinstance(statement, ast.Assign) else [statement.target]
                for target in targets:
                    if not isinstance(target, ast.Name):
                        node_error('Activity 的字段只支持名字（entry = "0"）', target)
                    if target.id in META_FIELDS:
                        # 元数据常量只能写在模块顶层，否则「清单说窗口」这条
                        # 分工就被绕过去了。
                        node_error('%s 只能写在模块顶层（清单负责窗口描述）' % target.id,
                                   target)
                    self.add_member(target.id, statement)
                    if isinstance(statement.value, ast.Constant):
                        self.field_defaults.append(
                            (target.id, self.constant(statement.value)))
                    elif isinstance(statement.value, (ast.List, ast.Dict)):
                        self.field_defaults.append(
                            (target.id, self.expr(statement.value)))
                    else:
                        node_error('Activity 字段的初值必须是字面量（数字、字符串、'
                                   'True/False/None、列表、字典）；需要计算请放进 onCreate',
                                   statement.value)
            elif isinstance(statement, (ast.Pass, ast.Expr)):
                # 文档字符串与空类体都是合法的
                pass
            else:
                node_error('Activity 类体里只允许 def 与字段赋值，不支持 %s'
                           % describe_statement(statement), statement)

    def scan_self_members(self, func):
        """扫出方法体里所有 `self.xxx = ...` 的 xxx，登记为成员。

        必须先扫后生成：`name_ref` 在生成表达式时就要知道哪些名字是成员，
        否则 `self.count` 会被当成不存在的名字报错。
        """
        for inner in ast.walk(func):
            targets = []
            if isinstance(inner, ast.Assign):
                targets = inner.targets
            elif isinstance(inner, ast.AugAssign):
                targets = [inner.target]
            elif isinstance(inner, ast.AnnAssign):
                targets = [inner.target]
            for target in targets:
                if (isinstance(target, ast.Attribute) and
                        isinstance(target.value, ast.Name) and
                        target.value.id == 'self'):
                    self.add_member(target.attr, inner)

    def declare_method(self, func):
        if func.decorator_list:
            node_error('不支持装饰器：脚本里没有一等函数，装饰器无从生效',
                       func.decorator_list[0])
        args = self.params_of_raw(func)
        if not args or args[0].arg != 'self':
            node_error('Activity 的方法第一个参数必须是 self（def %s(self, ...)）'
                       % func.name, func)
        self.method_nodes.add(id(func))
        self.functions[func.name] = func

    def collect_import(self, node):
        """把 import 解析成扩展模块引用。

        为什么 import 必须是「模块级、未限定、只认清单里的模块」：
        产物是静态链接的原生程序，没有模块搜索路径、没有 .py 文件、
        也没有运行期符号解析。所以 import 的语义只能是「告诉构建系统
        要链哪个库」——这是编译期的决定，不是运行期的动作。

        支持两种形式：
            import fs               → fs.read_file(...)
            from fs import read_file → read_file(...)
        """
        if self.extension_index is None:
            node_error('没有配置扩展目录，import 无处可查。'
                       '给 pyuic 加 --ext-dir <目录>', node)

        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.asname:
                    node_error('不支持 import ... as ...：请直接用模块名（%s）'
                               % alias.name, node)
                self.use_module(alias.name, node)
            return

        # from X import a, b
        if node.level > 0:
            node_error('不支持相对导入（from . import ...）：产物没有包结构', node)
        module = node.module
        if not module:
            node_error('from 后面必须写模块名', node)
        extension = self.use_module(module, node)
        for alias in node.names:
            name = alias.name
            if name == '*':
                node_error('不支持 from %s import *：请显式列出要用的名字' % module, node)
            if alias.asname:
                node_error('不支持 import ... as ...（%s as %s）' % (name, alias.asname), node)
            if name not in extension.functions:
                node_error('模块 "%s" 没有 %s。可用：%s'
                           % (module, name, '、'.join(sorted(extension.functions))), node)
            # from 导入的名字变成裸名字，登记到「本文件的自由函数表」里。
            # 值记下来源模块，调用点解析时才能查回 cpp 符号。
            if name in self.imported_from or name in self.functions:
                node_error('名字 "%s" 与已有定义冲突（函数或另一个 import）' % name, node)
            self.imported_from[name] = (module, name)

    def use_module(self, module, node):
        """登记一次性使用到的扩展模块，并返回它的清单。"""
        extension = self.extension_index.find(module)
        if extension is None:
            node_error('没有名为 "%s" 的扩展模块。当前可用：\n%s'
                       % (module, self.extension_index.describe_available()), node)
        if module not in self.used_module_set:
            self.used_module_set.add(module)
            self.used_modules.append(module)
        self.module_names.add(module)
        return extension

    def record_call(self, module, function, node):
        """记下「哪个扩展函数在哪一行被调用了」。used.json 靠它产出。"""
        sites = self.extension_sites.setdefault(module, {})
        sites.setdefault(function, []).append(
            (self.filename, getattr(node, 'lineno', 0),
             getattr(node, 'col_offset', 0) + 1))

    def extension_call(self, extension, module, function, node):
        """生成一次扩展函数调用。

        实参个数在构建期校验：清单里写了 arity，运行期就没有类型检查了，
        这里放过一个多传或少传的参数，症状会是 C++ 编译期的一堆模板报错，
        或者更糟——DLL 边界上的栈错位。
        """
        declared = extension.functions[function]
        cpp_name, low, high = declared
        count = len(node.args)
        if count < low or (high is not None and count > high):
            if low == high:
                node_error('"%s.%s" 需要 %d 个参数，这里给了 %d 个'
                           % (module, function, low, count), node)
            node_error('"%s.%s" 需要 %d~%d 个参数，这里给了 %d 个'
                       % (module, function, low, high, count), node)

        self.record_call(module, function, node)
        args = ', '.join(self.expr(a) for a in node.args)
        return '%s(%s)' % (cpp_name, args)

    def collect_assign(self, node):
        # ast.Assign 用 targets（支持 a = b = 1），ast.AnnAssign 用 target
        if isinstance(node, ast.Assign):
            targets = node.targets
        else:
            targets = [node.target]
        for target in targets:
            if not isinstance(target, ast.Name):
                node_error('模块顶层只支持给名字赋值（x = ...）', target)
            name = target.id
            if name in META_FIELDS:
                self.meta_names.add(name)
                self.meta_assign_node[name] = node
                self.meta_lines.append(self.meta_literal(name, node.value))
                continue
            self.add_member(name, node)

    def is_meta_assign(self, node):
        """这条顶层赋值是不是在给元数据常量赋值（由 describe 消费）。"""
        if isinstance(node, ast.Assign):
            return any(isinstance(t, ast.Name) and t.id in self.meta_names for t in node.targets)
        if isinstance(node, ast.AnnAssign):
            return isinstance(node.target, ast.Name) and node.target.id in self.meta_names
        return False

    def collect_augassign(self, node):
        if not isinstance(node.target, ast.Name):
            node_error('模块顶层的增强赋值只支持名字（x += 1）', node.target)
        self.add_member(node.target.id, node)

    def add_member(self, name, node):
        if name in FREE_FUNCTIONS or name in SCRIPT_MEMBERS or name in METHODS:
            node_error('"%s" 是脚本 API 的名字，不能当变量用；请换个名字' % name, node)
        if name not in self.member_set:
            self.member_set.add(name)
            self.members.append(name)

    def meta_literal(self, name, value):
        """元数据必须能在构建期求值，所以只接受字面量。"""
        field, kind = META_FIELDS[name]
        if kind == 'string':
            if not isinstance(value, ast.Constant) or not isinstance(value.value, str):
                node_error('%s 必须是字符串字面量（pyuic 在构建期就要读它）' % name, value)
            return 'spec.%s = %s;' % (field, cpp_string(value.value))
        if kind == 'int':
            if not isinstance(value, ast.Constant) or not isinstance(value.value, int):
                node_error('%s 必须是整数字面量' % name, value)
            return 'spec.%s = %d;' % (field, value.value)
        if kind == 'bool':
            if not isinstance(value, ast.Constant) or not isinstance(value.value, bool):
                node_error('%s 必须是 True 或 False' % name, value)
            return 'spec.%s = %s;' % (field, 'true' if value.value else 'false')
        if kind == 'string_list':
            if not isinstance(value, (ast.List, ast.Tuple)):
                node_error('%s 必须是字符串列表，如 ["txt"]' % name, value)
            items = []
            for element in value.elts:
                if not isinstance(element, ast.Constant) or not isinstance(element.value, str):
                    node_error('%s 里只能放字符串' % name, element)
                items.append(cpp_string(element.value))
            return 'spec.%s = {%s};' % (field, ', '.join(items))
        node_error('未知的元数据类型 %s' % kind, value)

    # ── 类声明 ──

    def emit_class(self, out):
        out.append('class %s final : public py::Script {' % self.class_name)
        out.append(' public:')
        out.append('  %s(const %s&) = delete;' % (self.class_name, self.class_name))
        out.append('  %s& operator=(const %s&) = delete;' % (self.class_name, self.class_name))
        out.append('  %s() = default;' % self.class_name)
        out.append('')

        # 普通 def：全部生成成 py::Value 成员函数。先全声明，
        # 因为脚本里的函数可以互相调用，也可以被前面的绑定引用。
        for name in self.functions:
            if name in ('build', 'onCreate'):
                continue
            func = self.functions[name]
            out.append('  %s' % self.signature(name, func))
        if any(name not in ('build', 'onCreate') for name in self.functions):
            out.append('')

        out.append('  void describe(py::AppSpec& spec) override;')
        out.append('  void initState() override;')
        out.append('  void build() override;')
        out.append('')
        out.append(' private:')
        # build() 内部的两个钩子。做成成员而不是直接展开在 build() 里：
        # build() 自己还要塞字段初值，混在一起读不出「哪几段是脚本的」。
        out.append('  void runBuildHook();')
        out.append('  void runOnCreate();')
        out.append('')
        out.append(' public:')
        if 'on_frame' in self.functions:
            out.append('  void onFrame() override;')
            out.append('  bool wantsFrameTimer() const override { return true; }')
        if 'on_resize' in self.functions:
            out.append('  void onResize(int argWidth, int argHeight) override;')
        if 'on_key' in self.functions:
            out.append('  bool onKeyDown(const Key& argKey, const Modifiers& argMods) override;')
        if 'on_char' in self.functions:
            out.append('  bool onChar(std::uint32_t argCodepoint) override;')
        if 'on_close' in self.functions:
            out.append('  bool onCloseRequest() override;')
        if 'on_file_drop' in self.functions:
            out.append('  void onFileDrop(const std::vector<std::string>& argPaths) override;')
        if 'open_file' in self.functions:
            out.append('  void openFile(const std::string& argPath) override;')
        if 'on_tray_activate' in self.functions:
            out.append('  void onTrayActivate() override;')
        if 'tray_menu' in self.functions:
            out.append('  std::vector<MenuItem> trayMenu() override;')
        if 'on_tray_command' in self.functions:
            out.append('  void onTrayCommand(const std::string& argId) override;')

        out.append('')
        out.append(' private:')
        if self.members:
            for name in self.members:
                out.append('  py::Value %s_;' % name)
        else:
            out.append('  // （脚本没有模块级变量）')
            out.append('  int unused_ = 0;')
        out.append('};')

    def signature(self, name, func, with_defaults=True):
        """函数声明与定义共用。定义时必须带上类名限定，否则会生成自由函数。

        默认参数只出现在**声明**里：C++ 不允许在类外定义处重复默认值，
        所以 definition() 传 with_defaults=False。
        """
        params = []
        for arg in self.params_of(func):
            params.append('const py::Value& %s' % arg.arg)
        if func.args.vararg is not None:
            node_error('不支持 *args：脚本里的函数参数个数应当固定', func)
        if func.args.kwarg is not None:
            node_error('不支持 **kwargs', func)
        if func.args.kwonlyargs:
            node_error('不支持仅关键字参数', func)

        if with_defaults and func.args.defaults:
            # 默认值必须是字面量：CPython 在 def 时求值一次，而生成代码把它
            # 放在调用点的默认实参里。非字面量的默认值（如 `n=len(a)`）
            # 语义不同，宁可在构建期报错也不要静默改变时机。
            first = len(params) - len(func.args.defaults)
            for offset, default in enumerate(func.args.defaults):
                position = first + offset
                if not isinstance(default, ast.Constant):
                    node_error('默认参数只能是字面量（如 n=1、"x"），'
                               '因为生成代码在调用点展开它', default)
                params[position] += ' = %s' % self.constant(default)

        return 'py::Value %s(%s);' % (name, ', '.join(params))

    @staticmethod
    def params_of_raw(func):
        """位置参数：`/` 之前的 posonlyargs 同样是位置参数。

        只读 args.args 会把 `def f(a, /)` 看成零参数，接着报「未定义的变量 a」——
        错得莫名其妙。CPython 把两者分开只是为了限制是否允许按关键字传，
        而生成代码本来就不支持关键字参数。
        """
        return list(func.args.posonlyargs) + list(func.args.args)

    def params_of(self, func):
        """对外可见的形参：Activity 方法的 `self` 不参与生成。

        生成的 C++ 只有一个实例，`self` 与 `this` 是同一个东西。把它留在
        签名里，调用点就得凭空造一个实参，而脚本里 `self` 从不出现在调用的
        实参列表里——去掉它才是「一个脚本一个类」这个事实的忠实映射。
        """
        args = self.params_of_raw(func)
        if self.is_method(func):
            args = args[1:]
        return args

    def is_method(self, func):
        return id(func) in self.method_nodes


    def definition(self, name, func, return_type='py::Value'):
        """定义形式：`py::Value Cls::name(...) {`。

        注意类名要加在函数名上，不能拼整个签名——否则会生成
        `Cls::py::Value name()`，把返回类型当成了作用域。
        """
        declaration = self.signature(name, func, with_defaults=False).rstrip(';')
        if return_type != 'py::Value':
            declaration = return_type + ' ' + declaration[len('py::Value '):]
        marker = name + '('
        head, tail = declaration.split(marker, 1)
        return '%s%s::%s(%s' % (head, self.class_name, name, tail)

    # ── 函数体 ──

    def emit_function(self, out, name, func, return_type='py::Value'):
        params = set(arg.arg for arg in self.params_of(func))
        # 函数级作用域：先扫出全部被赋值的名字，在函数开头统一声明。
        #
        # 必要性有两层：
        #   1. Python 里 `if x: n = 1` 之后 n 仍然存在；按 C++ 的块作用域
        #      生成，n 一出 if 就没了，后面读它会直接编译失败。
        #   2. 必须先扫后生成，否则「先用后赋值」的读取点还没登记，
        #      名字解析就会误报「未定义」。
        declared_globals = self.collect_globals(func)
        locals_found = set()
        for statement in func.body:
            self.collect_assigned(statement, locals_found)
        locals_found -= params
        # Python 规则：只要函数里出现了 `global x`，x 就**不是**局部量。
        # 不写 global 却赋值给模块级名字时，CPython 会新建一个局部变量，
        # 模块级那个纹丝不动——生成代码必须照做，否则同一份脚本在
        # CPython 与编译产物上行为不同。
        locals_found -= declared_globals

        # 不写 global 却赋值给模块级名字：这在 CPython 里是合法的（创建
        # 局部量），但几乎总是笔误——作者以为在改模块级变量。报错比
        # 「改了半天没生效」好排查得多。
        shadowed = set(n for n in locals_found if n in self.member_set)
        if shadowed:
            node_error(
                '函数里给模块级变量 %s 赋值，但没有写 global 声明。'
                'CPython 会把它当成新的局部变量，模块级的值不会变；'
                '要改模块级变量请先写 `global %s`'
                % ('、'.join(sorted(shadowed)), sorted(shadowed)[0]), func)

        self.scope_globals = declared_globals
        self.scope_locals = set(params) | locals_found
        self.locals = [set(self.scope_locals)]
        body = self.block(func.body, 2)
        self.locals = []
        self.scope_globals = set()
        self.scope_locals = set()

        out.append('%s {' % self.definition(name, func, return_type))
        # 脚本里留空参数是常态（比如 on_key 只关心 key 不关心 mods）。
        # C++ 的 /W4 会对未使用参数报警，而这不是错误，显式吞掉。
        used = self.collect_used(func.body)
        for param in self.params_of(func):
            if param.arg not in used:
                out.append('  (void)%s;' % param.arg)
        if locals_found:
            # 局部变量统一用 py::Value，初值是 None。与 Python 里「变量在
            # 赋值前不存在」不同，但读一个未赋值变量本身就是 bug，
            # 给 None 比给随机值安全，也不需要额外的存在跟踪。
            for local in sorted(locals_found):
                out.append('  py::Value %s;' % local)
        if body:
            out.append(body)
        # 只有函数体不是以 return 结尾时才补：否则生成不可达代码（C4702）。
        # void 的钩子（build）没有返回值语义，补 return 反而是语法错误。
        if return_type == 'py::Value' and not self.ends_with_return(func.body):
            out.append('  return py::Value();')
        out.append('}')

    def collect_globals(self, func):
        """函数体里 `global a, b` 声明的名字。

        只在最外层语句里找：global 必须出现在函数体的顶层（CPython 允许它
        在 if 里，但那样太容易被误读，这里只认顶层，写错时报错更直接）。
        """
        found = set()
        for statement in func.body:
            if isinstance(statement, ast.Global):
                found.update(statement.names)
        for name in found:
            if name not in self.member_set:
                node_error('global %s：模块顶层没有这个变量' % name, func)
        return found

    def collect_target(self, target, found):
        if isinstance(target, ast.Name):
            found.add(target.id)
        elif isinstance(target, ast.Tuple):
            for element in target.elts:
                self.collect_target(element, found)

    def collect_used(self, node):
        """收集表达式里被读取的名字，用来判断哪些参数确实没被用到。"""
        used = set()
        # ast.walk 只接受单个节点，传列表要先逐个展开
        for root in (node if isinstance(node, list) else [node]):
            for child in ast.walk(root):
                if isinstance(child, ast.Name) and isinstance(child.ctx, ast.Load):
                    used.add(child.id)
        return used

    def collect_assigned(self, node, found):
        """收集一个块里所有「被赋值」的名字（含嵌套块与循环变量）。"""
        for statement in ast.walk(node):
            if isinstance(statement, ast.Assign):
                for target in statement.targets:
                    self.collect_target(target, found)
            elif isinstance(statement, (ast.AnnAssign, ast.AugAssign)):
                self.collect_target(statement.target, found)
            elif isinstance(statement, ast.For):
                self.collect_target(statement.target, found)
        return found

    def ends_with_return(self, body):
        if not body:
            return False
        last = body[-1]
        if isinstance(last, ast.Return):
            return True
        if isinstance(last, ast.If):
            if last.orelse and self.ends_with_return(last.body) and self.ends_with_return(last.orelse):
                return True
        return False

    # ── 语句 ──

    def block(self, statements, indent):
        lines = []
        for node in statements:
            lines.extend(self.statement(node, indent))
        return '\n'.join(lines)

    def statement(self, node, indent):
        pad = ' ' * indent
        if isinstance(node, ast.Pass):
            return [pad + ';']

        if isinstance(node, ast.Expr):
            # yield / await 不是「没有副作用的表达式」：丢掉它们等于悄悄改了
            # 程序语义。Expr 分支只在确认无害时才允许返回空。
            if isinstance(node.value, (ast.Yield, ast.YieldFrom)):
                node_error('不支持 yield：脚本里没有生成器', node.value)
            if isinstance(node.value, ast.Await):
                node_error('不支持 await：脚本是单线程事件循环', node.value)
            # 只有调用可能有副作用；纯表达式直接丢掉，避免生成 `(void)x;` 噪声
            if isinstance(node.value, ast.Call):
                return [pad + '(void)%s;' % self.expr(node.value)]
            return []

        if isinstance(node, ast.Assign):
            return self.assign(node, indent)

        if isinstance(node, ast.AnnAssign):
            if node.value is None:
                return []
            return self.assign(node, indent)

        if isinstance(node, ast.AugAssign):
            # augmented() 返回整条语句（下标目标要读-改-写，不止是一个表达式）
            return [pad + self.augmented(node)]

        if isinstance(node, ast.Return):
            if node.value is None:
                return [pad + 'return py::Value();']
            return [pad + 'return %s;' % self.expr(node.value)]

        if isinstance(node, ast.If):
            return [self.if_chain(node, indent)]

        if isinstance(node, ast.While):
            lines = [pad + 'while (%s.toBool()) {' % self.expr(node.test)]
            lines.append(self.block(node.body, indent + 2))
            lines.append(pad + '}')
            if node.orelse:
                node_error('不支持 while ... else', node)
            return lines

        if isinstance(node, ast.For):
            return self.for_loop(node, indent)

        if isinstance(node, ast.Break):
            return [pad + 'break;']

        if isinstance(node, ast.Continue):
            return [pad + 'continue;']

        if isinstance(node, ast.Global):
            # 模块级变量已经是成员，函数里直接读写就是「全局」语义，
            # 所以 global 声明本身不生成任何代码。
            return []

        if isinstance(node, ast.Nonlocal):
            node_error('不支持 nonlocal；嵌套函数本身也不支持', node)

        if isinstance(node, ast.Assert):
            node_error('不支持 assert；请在脚本里写 if + 显式处理', node)

        if isinstance(node, ast.Raise):
            node_error('不支持 raise：没有异常通道。请用返回值表示失败', node)

        if isinstance(node, (ast.With, ast.Try)):
            node_error('不支持 %s 语句：%s'
                       % (describe_statement(node), describe_remedy(node)), node)

        if isinstance(node, ast.FunctionDef):
            node_error(
                '不支持嵌套函数 "%s"；请把它移到模块顶层' % node.name, node)

        if isinstance(node, ast.Delete):
            node_error('不支持 del；容器用 .pop()/.clear()，变量不用管', node)

        node_error('不支持语句 %s' % type(node).__name__, node)

    def if_chain(self, node, indent):
        pad = ' ' * indent
        lines = ['%sif (%s.toBool()) {' % (pad, self.expr(node.test))]
        lines.append(self.block(node.body, indent + 2))

        orelse = node.orelse
        while orelse:
            # elif 在 AST 里是「orelse 只有一条 If」，展开成 else if
            if len(orelse) == 1 and isinstance(orelse[0], ast.If):
                inner = orelse[0]
                lines.append('%s} else if (%s.toBool()) {' % (pad, self.expr(inner.test)))
                lines.append(self.block(inner.body, indent + 2))
                orelse = inner.orelse
                continue
            lines.append('%s} else {' % pad)
            lines.append(self.block(orelse, indent + 2))
            orelse = None
        lines.append('%s}' % pad)
        return '\n'.join(lines)

    def assign(self, node, indent):
        pad = ' ' * indent
        value = self.expr(node.value)

        # ast.Assign 用 targets（支持 a = b = 1），ast.AnnAssign 用 target
        targets = node.targets if isinstance(node, ast.Assign) else [node.target]
        lines = []
        for target in targets:
            lines.extend(self.assign_one(target, value, pad, node))
        return lines

    def assign_one(self, target, value, pad, node):
        if isinstance(target, ast.Name):
            name = target.id
            ref = self.name_ref(name)
            self.declare_local(name, node)
            return [pad + '%s = %s;' % (ref, value)]

        # self.xxx = v：Activity 的字段。self 只是写法上的前缀（见 attribute），
        # 落地就是成员赋值。
        if isinstance(target, ast.Attribute) and self.is_self(target.value):
            if target.attr not in self.member_set:
                self.add_member(target.attr, node)
            return [pad + '%s_ = %s;' % (target.attr, value)]

        # 下标赋值：list[i] = v / dict[k] = v
        if isinstance(target, ast.Subscript):
            container = self.expr(target.value)
            if isinstance(target.slice, ast.Slice):
                node_error('不支持切片赋值（x[a:b] = ...）', target)
            index = self.expr(target.slice)
            return [pad + 'py::setItem(%s, %s, %s);' % (container, index, value)]

        if isinstance(target, ast.Tuple):
            node_error('不支持元组解包赋值（a, b = ...）；请分开写两行', target)

        node_error('不支持这种赋值目标（只支持名字、self.xxx、list[i]、dict[k]）', target)

    @staticmethod
    def is_self(node):
        return isinstance(node, ast.Name) and node.id == 'self'

    def target_of(self, target, node):
        """赋值目标 → 可直接写在等号左边的 C++ 表达式。

        `x += 1` 与 `d[k] += 1` 都要能成立。名字要经 name_ref 才会映射到
        成员（模块级变量）；下标则按可寻址形式展开成一次求值的表达式——
        `py::at(d, k) += 1` 是错的（at 返回副本），所以这里对下标返回
        一个 **赋值函数调用** 形式，由 augmented() 统一处理。
        """
        if isinstance(target, ast.Name):
            self.declare_local(target.id, node)
            return self.name_ref(target.id)
        if isinstance(target, ast.Attribute) and self.is_self(target.value):
            if target.attr not in self.member_set:
                self.add_member(target.attr, node)
            return '%s_' % target.attr
        node_error('增强赋值的左侧只支持名字、self.xxx 或下标（x += 1 / d[k] += 1）',
                   target)

    def augmented(self, node):
        target = node.target
        value = self.expr(node.value)
        op = node.op
        table = {
            ast.Add: 'add', ast.Sub: 'sub', ast.Mult: 'mul', ast.Div: 'div',
            ast.FloorDiv: 'floorDiv', ast.Mod: 'mod', ast.Pow: 'power',
            ast.BitAnd: 'bitAnd', ast.BitOr: 'bitOr', ast.BitXor: 'bitXor',
            ast.LShift: 'shiftLeft', ast.RShift: 'shiftRight',
        }
        name = None
        for kind, candidate in table.items():
            if isinstance(op, kind):
                name = candidate
                break
        if name is None:
            node_error('不支持增强赋值运算符 %s' % type(op).__name__, node)

        # 下标目标要「读-改-写」：py::at 取的是副本，不能直接当左值。
        # 用 setItem 重新写回去，语义与 CPython 的 d[k] += v 一致。
        if isinstance(target, ast.Subscript):
            if isinstance(target.slice, ast.Slice):
                node_error('不支持切片赋值（x[a:b] = ...）', target)
            container = self.expr(target.value)
            index = self.expr(target.slice)
            current = 'py::at(%s, %s)' % (container, index)
            return 'py::setItem(%s, %s, py::%s(%s, %s));' % (
                container, index, name, current, value)
        # 下标的读-改-写用 setItem 重新写回；其余目标都是可寻址的成员/局部量，
        # 直接读同一表达式即可（self.xxx 已被 target_of 擦成成员名）。
        left = self.target_of(target, node)
        return '%s = py::%s(%s, %s);' % (left, name, left, value)

    def for_loop(self, node, indent):
        pad = ' ' * indent
        if not isinstance(node.target, ast.Name):
            node_error('for 循环变量必须是单个名字', node.target)
        if node.orelse:
            node_error('不支持 for ... else', node)

        # for i in range(...) 直接编成 C++ 的 for：语义一致，也不用建列表
        if (isinstance(node.iter, ast.Call)
                and isinstance(node.iter.func, ast.Name)
                and node.iter.func.id == 'range'
                and 1 <= len(node.iter.args) <= 3
                and not node.iter.keywords):
            return self.range_for(node, indent)

        var = node.target.id
        # 循环变量是局部量；但它可能同名于成员，所以用一个独立名字，
        # 再在循环体开头赋值。这样「循环结束后变量还在」的 Python 语义
        # （虽然少见）也不会出错。
        loop_var = 'item_%d' % node.lineno
        lines = [pad + 'for (const py::Value& %s : py::iterate(%s)) {' %
                 (loop_var, self.expr(node.iter))]
        self.declare_local(var, node)
        lines.append(pad + '  %s = %s;' % (self.name_ref(var), loop_var))
        lines.append(self.block(node.body, indent + 2))
        lines.append(pad + '}')
        return lines

    def range_for(self, node, indent):
        pad = ' ' * indent
        args = node.iter.args
        var = node.target.id

        start = '0LL'
        stop = None
        step = '1LL'
        if len(args) == 1:
            stop = '%s.toInt()' % self.expr(args[0])
        else:
            start = '%s.toInt()' % self.expr(args[0])
            stop = '%s.toInt()' % self.expr(args[1])
            if len(args) == 3:
                step = '%s.toInt()' % self.expr(args[2])

        # 步长要在循环里复用，先绑一个临时量：直接内联会在每次迭代
        # 重新求值一次表达式，副作用会跑多遍。
        step_var = 'step_%d' % node.lineno
        index_var = 'index_%d' % node.lineno

        self.declare_local(var, node)
        lines = [
            pad + 'for (long long %s = %s, %s = %s; py::rangeContinue(%s, %s, %s); %s += %s) {' %
            (index_var, start, step_var, step, index_var, stop, step_var, index_var, step_var),
        ]
        # 步长为 0 时 range 在 Python 里是空循环；这里每次都会判 false，
        # 不会死循环（rangeContinue 里 step >= 0 走 index < stop）。
        lines.append(pad + '  %s = py::Value(%s);' % (self.name_ref(var), index_var))
        lines.append(self.block(node.body, indent + 2))
        lines.append(pad + '}')
        return lines

    # ── 作用域 ──

    def declare_local(self, name, node):
        """Python 是函数级作用域：局部变量在第一次赋值处「出现」，
        但整个函数内都可见。生成的 C++ 用块作用域，所以必须在函数
        开头统一声明。这里只登记，声明由 emit_function 统一输出。"""
        # 模块级名字是成员，除非本函数用 global 把它拉进来（那也还是成员）。
        # 两种情况都直接写成员，不声明局部量。
        if name in self.member_set:
            return
        if self.locals:
            self.locals[-1].add(name)

    def name_ref(self, name):
        """脚本里的名字 → C++ 里的写法。

        模块级变量是生成类的成员，带 `_` 后缀（与项目命名约定一致）；
        局部量与参数原样输出。
        """
        if name in self.member_set:
            return '%s_' % name
        return name

    def is_local(self, name):
        for scope in self.locals:
            if name in scope:
                return True
        return False

    # ── 表达式 ──

    def expr(self, node):
        if isinstance(node, ast.Constant):
            return self.constant(node)

        if isinstance(node, ast.Name):
            if node.id == 'True':
                return 'py::Value(true)'
            if node.id == 'False':
                return 'py::Value(false)'
            if node.id == 'None':
                return 'py::Value()'
            # `self` 只以 self.xxx / self.foo() 的形式出现，两者都在
            # attribute / method_call 里被消化掉。单独把 self 当值用（赋值给
            # 别人、放进列表）没有对应语义：C++ 侧的 this 不是 py::Value。
            if node.id == 'self':
                node_error('不支持把 self 当值使用；只在 self.字段 / self.方法() 里出现',
                           node)
            if node.id not in self.member_set and not self.is_local(node.id):
                # 允许引用模块级的函数名（用于事件绑定），其余是笔误
                if node.id not in self.functions and node.id not in FREE_FUNCTIONS:
                    node_error('未定义的变量 "%s"（拼写错误？）' % node.id, node)
            return self.name_ref(node.id)

        if isinstance(node, ast.BinOp):
            return self.binop(node)

        if isinstance(node, ast.BoolOp):
            return self.boolop(node)

        if isinstance(node, ast.UnaryOp):
            return self.unaryop(node)

        if isinstance(node, ast.Compare):
            return self.compare(node)

        if isinstance(node, ast.IfExp):
            # 三元表达式：用 py::iff(cond, a, b)。不能编成 C++ 的 ?:，
            # 因为两个分支的类型都是 py::Value，?: 会退化成拷贝，
            # 而 Python 只求值其中一个分支——用函数才能保证惰性。
            return 'py::iff(%s, [&]{ return %s; }, [&]{ return %s; })' % (
                self.expr(node.test), self.expr(node.body), self.expr(node.orelse))

        if isinstance(node, ast.Call):
            return self.call(node)

        if isinstance(node, ast.Attribute):
            return self.attribute(node)

        if isinstance(node, ast.Subscript):
            return self.subscript(node)

        if isinstance(node, ast.Slice):
            lower = 'py::Value()' if node.lower is None else self.expr(node.lower)
            upper = 'py::Value()' if node.upper is None else self.expr(node.upper)
            return 'py::slice(%s, %s, %s)' % (self.expr(node), lower, upper)

        if isinstance(node, ast.List):
            return 'py::Value::list(py::Value::List{%s})' % ', '.join(
                self.expr(e) for e in node.elts)

        if isinstance(node, ast.Tuple):
            return 'py::Value::list(py::Value::List{%s})' % ', '.join(
                self.expr(e) for e in node.elts)

        if isinstance(node, ast.Set):
            node_error('不支持集合字面量；请用列表', node)

        if isinstance(node, ast.Dict):
            return self.dict_literal(node)

        if isinstance(node, ast.JoinedStr):
            return self.fstring(node)

        if isinstance(node, ast.FormattedValue):
            return self.expr(node.value)

        if isinstance(node, ast.ListComp):
            node_error('不支持列表推导；请用 for 循环 + .append()', node)

        if isinstance(node, ast.GeneratorExp):
            node_error('不支持生成器表达式；请用 for 循环', node)

        if isinstance(node, ast.DictComp):
            node_error('不支持字典推导；请用 for 循环 + d[k] = v', node)

        if isinstance(node, ast.Lambda):
            node_error('不支持 lambda；请定义一个具名函数', node)

        if isinstance(node, ast.Starred):
            node_error('不支持 *展开', node)

        if isinstance(node, ast.NamedExpr):
            node_error('不支持海象运算符 :=', node)

        node_error('不支持表达式 %s' % type(node).__name__, node)

    def constant(self, node):
        value = node.value
        if value is None:
            return 'py::Value()'
        if isinstance(value, bool):
            return 'py::Value(%s)' % ('true' if value else 'false')
        if isinstance(value, int):
            return 'py::Value(%dLL)' % value
        if isinstance(value, float):
            return 'py::Value(%s)' % cpp_double(value)
        if isinstance(value, str):
            return 'py::Value(%s)' % cpp_string(value)
        if isinstance(value, bytes):
            node_error('不支持 bytes 字面量；请用字符串', node)
        if value is Ellipsis:
            node_error('不支持 ... 字面量', node)
        node_error('不支持常量 %r' % (value,), node)

    def binop(self, node):
        left = self.expr(node.left)
        right = self.expr(node.right)
        op = node.op
        table = {
            ast.Add: 'add', ast.Sub: 'sub', ast.Mult: 'mul', ast.Div: 'div',
            ast.FloorDiv: 'floorDiv', ast.Mod: 'mod', ast.Pow: 'power',
            ast.BitAnd: 'bitAnd', ast.BitOr: 'bitOr', ast.BitXor: 'bitXor',
            ast.LShift: 'shiftLeft', ast.RShift: 'shiftRight',
        }
        for kind, name in table.items():
            if isinstance(op, kind):
                return 'py::%s(%s, %s)' % (name, left, right)
        if isinstance(op, ast.MatMult):
            node_error('不支持矩阵乘法 @', node)
        node_error('不支持运算符 %s' % type(op).__name__, node)

    def boolop(self, node):
        # Python 的 and/or 返回操作数本身，且**短路求值**。
        # 生成代码用 lambda 传右边，保证它只在需要时求值，否则
        # `if items and items[0]` 会在 items 为空时越界。
        values = node.values
        if len(values) < 2:
            node_error('and/or 至少需要两个操作数', node)
        result = self.expr(values[0])
        function = 'landLazy' if isinstance(node.op, ast.And) else 'lorLazy'
        for value in values[1:]:
            result = 'py::%s(%s, [&]{ return %s; })' % (function, result, self.expr(value))
        return result

    def unaryop(self, node):
        operand = self.expr(node.operand)
        if isinstance(node.op, ast.Not):
            return 'py::lnot(%s)' % operand
        if isinstance(node.op, ast.USub):
            return 'py::neg(%s)' % operand
        if isinstance(node.op, ast.UAdd):
            return operand
        if isinstance(node.op, ast.Invert):
            return 'py::invert(%s)' % operand
        node_error('不支持一元运算符 %s' % type(node.op).__name__, node)

    def compare(self, node):
        # 链式比较 a < b < c 要保证每个中间项只求值一次
        ops = []
        left = self.expr(node.left)
        for op, comparator in zip(node.ops, node.comparators):
            right = self.expr(comparator)
            ops.append(self.compare_one(op, left, right, node))
            left = right
        if len(ops) == 1:
            # 单次比较本身就是 py::Value，交给外层（if/while 会补 .toBool()）
            return ops[0]
        # 链式比较整体是 bool 结果，但外层还会再补一次 .toBool()，
        # 所以这里保持返回 Value，内部合成一个 py::Value(bool)。
        joined = ' && '.join('%s.toBool()' % op for op in ops)
        return 'py::Value(static_cast<bool>(%s))' % joined

    def compare_one(self, op, left, right, node):
        table = {
            ast.Eq: 'eq', ast.NotEq: 'ne', ast.Lt: 'lt', ast.LtE: 'le',
            ast.Gt: 'gt', ast.GtE: 'ge',
        }
        for kind, name in table.items():
            if isinstance(op, kind):
                return 'py::%s(%s, %s)' % (name, left, right)
        # `x in y` 的签名是 contains(容器, 元素)，参数顺序要反
        if isinstance(op, ast.In):
            return 'py::contains(%s, %s)' % (right, left)
        if isinstance(op, ast.NotIn):
            return 'py::lnot(py::contains(%s, %s))' % (right, left)
        if isinstance(op, ast.Is):
            return 'py::isSame(%s, %s)' % (left, right)
        if isinstance(op, ast.IsNot):
            return 'py::lnot(py::isSame(%s, %s))' % (left, right)
        node_error('不支持比较运算符 %s' % type(op).__name__, node)

    def slice_index(self, node, owner):
        """下标表达式。切片单独处理：它的结果不是「取元素」而是「取子序列」。"""
        if isinstance(node, ast.Slice):
            node_error('切片不能直接作为单个下标使用', owner)
        return self.expr(node)

    def subscript(self, node):
        """x[i] 与 x[a:b] 走不同的运行期函数。"""
        if isinstance(node.slice, ast.Slice):
            lower = 'py::Value()' if node.slice.lower is None else self.expr(node.slice.lower)
            upper = 'py::Value()' if node.slice.upper is None else self.expr(node.slice.upper)
            return 'py::slice(%s, %s, %s)' % (self.expr(node.value), lower, upper)
        return 'py::at(%s, %s)' % (self.expr(node.value), self.expr(node.slice))

    def attribute(self, node):
        """`self.xxx` 是成员；其它属性访问没有对象模型。

        为什么要专门支持 self：Activity 的写法里字段天然带 self 前缀
        （Android 一贯如此）。既然后端只有一个实例，`self.entry` 与模块级的
        `entry` 就是同一个成员，生成时直接擦掉 self 前缀即可。
        """
        if isinstance(node.value, ast.Name) and node.value.id == 'self':
            if node.attr not in self.member_set:
                node_error('Activity 没有字段 "%s"：先在 __init__ 或类体里赋值'
                           '（self.%s = ...）' % (node.attr, node.attr), node)
            return '%s_' % node.attr
        node_error('不支持属性访问 ".%s"；控件方法请写成 find("id").set_text(...) 这样'
                   % node.attr, node)

    def dict_literal(self, node):
        if any(key is None for key in node.keys):
            node_error('不支持 **展开到字典字面量', node)
        keys = 'py::Value::list(py::Value::List{%s})' % ', '.join(
            self.expr(k) for k in node.keys)
        values = 'py::Value::list(py::Value::List{%s})' % ', '.join(
            self.expr(v) for v in node.values)
        return 'py::Value::dict(%s.asList(), %s.asList())' % (keys, values)

    def fstring(self, node):
        # f-string 编成「逐段 str() 后拼接」。
        #
        # 只支持不带格式说明、不带 !r/!a 的形式。原因是这两种写法不是
        # 「拼字符串」而是「格式化」：格式说明（`{x:>4}`、`{x:.2f}`）需要
        # 一整套迷你语言，!r 需要 repr 语义。静默忽略它们会得到
        # 「f"{v:>4}" 变成 "1"」这种看起来对、实际少了空格的输出——
        # 那是本节最不该出现的一类 bug，所以构建期直接报错。
        parts = []
        for value in node.values:
            if isinstance(value, ast.Constant):
                parts.append('py::Value(%s)' % cpp_string(str(value.value)))
                continue
            if not isinstance(value, ast.FormattedValue):
                node_error('不认识的 f-string 片段 %s' % type(value).__name__, value)
            # conversion: -1 表示没写，ord('s') 就是 str()，与默认行为等价。
            if value.conversion not in (-1, ord('s')):
                node_error('不支持 f-string 的 !r / !a：请写 str(...) 或 repr 到变量里',
                           value)
            if value.format_spec is not None:
                node_error('不支持 f-string 的格式说明（如 {x:>4} / {x:.2f}）：'
                           '请先用 format_text 或手工补位', value)
            parts.append('py::toText(%s)' % self.expr(value.value))
        if not parts:
            return 'py::Value("")'
        result = parts[0]
        for part in parts[1:]:
            result = 'py::add(%s, %s)' % (result, part)
        return result

    # ── 调用 ──

    def call(self, node):
        if node.keywords:
            node_error('不支持关键字参数；请按位置传参', node)

        if isinstance(node.func, ast.Attribute):
            return self.method_call(node)

        if not isinstance(node.func, ast.Name):
            node_error('只能调用函数名或控件方法', node)

        name = node.func.id

        # 脚本自己定义的函数
        if name in self.functions:
            args = [self.expr(a) for a in node.args]
            declared = self.functions[name]
            total = len(self.params_of(declared))
            # 默认参数让实参个数可以少于形参个数，但不能多于它。
            minimum = total - len(declared.args.defaults)
            if len(args) < minimum or len(args) > total:
                if minimum == total:
                    node_error('"%s" 需要 %d 个参数，这里给了 %d 个'
                               % (name, total, len(args)), node)
                node_error('"%s" 需要 %d~%d 个参数，这里给了 %d 个'
                           % (name, minimum, total, len(args)), node)
            return '%s(%s)' % (name, ', '.join(args))

        # 事件绑定：第二个参数必须是函数名
        if name in EVENT_BINDINGS:
            return self.event_binding(node, name)

        # Script 成员
        if name in SCRIPT_MEMBERS:
            function, low, high = SCRIPT_MEMBERS[name]
            self.check_arity(name, node, low, high)
            return '%s(%s)' % (function, ', '.join(self.expr(a) for a in node.args))

        # 自由函数
        if name in FREE_FUNCTIONS:
            function, low, high = FREE_FUNCTIONS[name]
            if function is None:
                node_error('range 只能用在 for 循环里（for i in range(10)）', node)
            self.check_arity(name, node, low, high)
            # round(x, n) 走另一个实现
            if name == 'round' and len(node.args) == 2:
                return 'py::roundDigits(%s, %s)' % (self.expr(node.args[0]),
                                                    self.expr(node.args[1]))
            return 'py::%s(%s)' % (function, ', '.join(self.expr(a) for a in node.args))

        # `from fs import read_file` 进来的裸名字
        if name in self.imported_from:
            module, function = self.imported_from[name]
            extension = self.extension_index.find(module)
            return self.extension_call(extension, module, function, node)

        node_error('未知的函数 "%s"' % name, node)

    def check_arity(self, name, node, low, high):
        count = len(node.args)
        if count < low or (high is not None and count > high):
            if low == high:
                node_error('"%s" 需要 %d 个参数，这里给了 %d 个' % (name, low, count), node)
            node_error('"%s" 需要 %d~%d 个参数，这里给了 %d 个' % (name, low, high, count), node)

    def event_binding(self, node, name):
        function, arity = EVENT_BINDINGS[name]
        if len(node.args) != 2:
            node_error('%s 需要 (控件, 函数名) 两个参数' % name, node)
        target = self.expr(node.args[0])
        handler = node.args[1]
        if not isinstance(handler, ast.Name):
            node_error('%s 的第二个参数必须是函数名（先 def 再引用）' % name, handler)
        handler_name = handler.id
        if handler_name not in self.functions:
            node_error('找不到函数 "%s"；先 def %s(...) 再绑定'
                       % (handler_name, handler_name), handler)
        declared = len(self.params_of(self.functions[handler_name]))
        if declared != arity:
            node_error('%s 作为回调必须接受 %d 个参数，但 "%s" 定义了 %d 个'
                       % (name, arity, handler_name, declared), handler)

        param = '(const py::Value& eventValue)' if arity == 1 else '()'
        passed = 'eventValue' if arity == 1 else ''
        return 'py::%s(%s, [this]%s { return %s(%s); })' % (
            function, target, param, handler_name, passed)

    def method_call(self, node):
        method = node.func.attr

        # self.foo(...) → foo(...)：Activity 里调自己的另一个方法。
        # 不加 this-> 前缀，因为结果与成员函数调用同形，读起来更接近脚本。
        if isinstance(node.func.value, ast.Name) and node.func.value.id == 'self':
            if method not in self.functions:
                node_error('Activity 没有方法 "%s"' % method, node.func)
            declared = self.functions[method]
            total = len(self.params_of(declared))
            minimum = total - len(declared.args.defaults)
            if len(node.args) < minimum or len(node.args) > total:
                if minimum == total:
                    node_error('"%s" 需要 %d 个参数，这里给了 %d 个'
                               % (method, total, len(node.args)), node)
                node_error('"%s" 需要 %d~%d 个参数，这里给了 %d 个'
                           % (method, minimum, total, len(node.args)), node)
            return '%s(%s)' % (method, ', '.join(self.expr(a) for a in node.args))

        # `fs.read_file(x)`：扩展模块的属性调用。必须在 METHODS 之前判定，
        # 否则模块名会被当成「某个值的字符串方法接收者」，报「不支持 .read_file」。
        if isinstance(node.func.value, ast.Name):
            receiver = node.func.value.id
            if receiver in self.module_names:
                extension = self.extension_index.find(receiver)
                if method not in extension.functions:
                    node_error('模块 "%s" 没有 %s。可用：%s'
                               % (receiver, method,
                                  '、'.join(sorted(extension.functions))), node.func)
                return self.extension_call(extension, receiver, method, node)

        if method not in METHODS:
            node_error('不支持的方法 ".%s"' % method, node.func)

        function, low, high = METHODS[method]
        self.check_arity('.' + method, node, low, high)

        receiver = self.expr(node.func.value)
        args = [self.expr(a) for a in node.args]
        return 'py::%s(%s%s)' % (function, receiver, ''.join(', ' + a for a in args))


# ═══════════════════════════════════════════════════════════
# 入口
# ═══════════════════════════════════════════════════════════

def class_name_of(stem):
    """calc.py → CalcScript。与 layoutc 的 identFromStem 同源。"""
    out = []
    upper = True
    for ch in stem:
        if not ch.isalnum():
            upper = True
            continue
        out.append(ch.upper() if upper and ch.islower() else ch)
        upper = False
    text = ''.join(out) or 'App'
    if text[0].isdigit():
        text = 'S' + text
    return text + 'Script'


def write_if_changed(path, content):
    """内容相同就不写：保住「只改了一个脚本才重编」的增量规则。"""
    try:
        with open(path, 'r', encoding='utf-8', newline='') as handle:
            if handle.read() == content:
                return
    except OSError:
        pass
    with open(path, 'w', encoding='utf-8', newline='') as handle:
        handle.write(content)


def main_source(class_name, stem, windows):
    """生成应用入口。

    这里刻意只有 pyui 的公开头：入口不该知道任何控件、任何平台类型。
    用户想接管入口就自己写一个，内容和这四行一样——所以「自写」不会成为
    一条需要长期维护的并行代码路径。

    为什么 Windows 上不用 int main：链接器一看到 main() 就按控制台程序处理，
    双击启动时**先弹一个黑框**，再出现窗口。图形入口是 wWinMain，配合
    CMake 的 WIN32 目标属性，进程才不带控制台子系统。

    参数也不在这里拆：wWinMain 给的是 UTF-16 命令行，拆它要 CommandLineToArgvW，
    那是平台细节。py::run(script) 会从平台层取，生成的入口只剩一句。

    windows 由构建系统传入（CMake 的 WIN32 判断），不在这里猜 sys.platform：
    交叉编译时「构建机是什么」与「产物跑在哪」是两件事。
    """
    if windows:
        entry = (
            '#include <windows.h>\n'
            '\n'
            'int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {\n'
            '  // 必须在建窗口之前：之后系统就按 96 DPI 虚拟化位图，再改是无效的。\n'
            '  pyui::enableHighDpi();\n'
            '\n'
            '  pyui::py::Script* script = pyui::py::create%s();\n'
            '  if (script == nullptr) return 1;\n'
            '  const int code = pyui::py::run(*script);\n'
            '  delete script;\n'
            '  return code;\n'
            '}\n')
    else:
        entry = (
            'int main(int argc, char** argv) {\n'
            '  pyui::enableHighDpi();\n'
            '\n'
            '  pyui::py::Script* script = pyui::py::create%s();\n'
            '  if (script == nullptr) return 1;\n'
            '  const int code = pyui::py::run(*script, argc, argv);\n'
            '  delete script;\n'
            '  return code;\n'
            '}\n')
    return (
        '// 由 pyuic 从 %s.py 生成，请勿手工修改\n'
        '//\n'
        '// 应用入口。之所以由生成器写，是为了让源码目录里只有 .py 与 .xml：\n'
        '// 入口只有一种正确写法，让每个应用各抄一份只会多出抄错的机会。\n'
        '// 需要自定义时，用 --no-main 关掉本文件，自己写一个同名入口。\n'
        '#include "pyui/platform/platform.h"\n'
        '#include "pyui/py_script.h"\n'
        '\n'
        '#include "%s.py.gen.h"\n'
        '\n' % (stem, stem) + entry % class_name)


def main(argv):
    parser = argparse.ArgumentParser(
        prog='pyuic',
        description='把 Python 脚本（子集）编译成 C++')
    parser.add_argument('--out', required=True, help='生成目录')
    parser.add_argument('--header', help='汇总头文件（可选）')
    parser.add_argument('--no-main', action='store_true',
                        help='不生成 <stem>.main.gen.cpp（想自己写入口时用）')
    parser.add_argument('--windows', action='store_true',
                        help='入口用 wWinMain（Windows 图形子系统，不带控制台）')
    parser.add_argument('--ext-dir', action='append', default=[],
                        help='扩展清单目录（可以有多个；每个子目录放 manifest.json）')
    parser.add_argument('--used', help='输出「用到了哪些扩展」的 JSON（给 pyui_extc）')
    parser.add_argument('sources', nargs='+', help='脚本路径')
    args = parser.parse_args(argv)

    # 扩展索引在这里装一次，全部脚本共用。装载失败（清单写错）不属于
    # 某个脚本的错误，所以直接退出，不要混进下面的 errors 列表。
    try:
        extension_index = extensions.load_dirs(args.ext_dir) if args.ext_dir else None
    except extensions.ExtensionError as error:
        print('pyuic: %s' % error, file=sys.stderr)
        return 1

    os.makedirs(args.out, exist_ok=True)
    header_lines = []
    errors = []
    used_sites = {}

    for source in args.sources:
        stem = os.path.splitext(os.path.basename(source))[0]
        try:
            with open(source, 'r', encoding='utf-8-sig') as handle:
                text = handle.read()
        except OSError as error:
            print('pyuic: 打不开 %s：%s' % (source, error), file=sys.stderr)
            return 1

        try:
            tree = ast.parse(text, filename=source)
        except SyntaxError as error:
            print('%s:%s:%s: error: %s' % (source, error.lineno, error.offset,
                                           error.msg), file=sys.stderr)
            return 1

        class_name = class_name_of(stem)
        # 清单：固定叫 manifest.xml，与脚本同目录。
        #
        # 固定名字（而不是 <stem>.json）是为了让「这个程序长什么样」永远在
        # 同一个位置：一个 exe 只可能有一份窗口声明，名字再跟着脚本变，
        # 找一个文件就要先知道脚本叫什么。
        manifest_path = os.path.join(os.path.dirname(os.path.abspath(source)),
                                     MANIFEST_FILENAME)
        try:
            manifest, _ = load_manifest(manifest_path)
        except ManifestError as error:
            if error.line is not None:
                print('%s:%s:%s: error: %s' % (manifest_path, error.line, error.column,
                                               error.message), file=sys.stderr)
            else:
                print('%s: error: %s' % (manifest_path, error.message), file=sys.stderr)
            errors.append((manifest_path, CompileError(error.message, 1, 1)))
            continue

        generator = Generator(class_name, source, extensions=extension_index,
                              manifest=manifest)
        try:
            code = generator.generate(tree)
        except CompileError as error:
            errors.append((source, error))
            continue

        # 汇总「用到了哪些扩展函数、在哪些位置」。多个脚本共用一个 used.json，
        # 所以这里按模块/函数合并，不覆盖。
        for module_name, functions in generator.extension_sites.items():
            target = used_sites.setdefault(module_name, {})
            for function_name, sites in functions.items():
                target.setdefault(function_name, []).extend(sites)

        write_if_changed(os.path.join(args.out, stem + '.py.gen.cpp'), code)

        # 每个脚本单独一个头。两条用途：
        #   1. 应用入口（下面生成的 <stem>.main.gen.cpp）按文件名 include；
        #   2. 想自己写 C++ 宿主的高级用户，也只需要这一个头。
        header = (
            '// 由 pyuic 生成，请勿手工修改\n'
            '#pragma once\n\n'
            'namespace pyui {\nnamespace py {\n\n'
            'class Script;\n\n'
            'Script* create%s();\n\n'
            '}  // namespace py\n}  // namespace pyui\n' % class_name)
        write_if_changed(os.path.join(args.out, stem + '.py.gen.h'), header)
        header_lines.append('py::Script* create%s();' % class_name)

        # 应用入口。为什么由生成器写、而不是让每个应用自带一个 main.cpp：
        #
        # 目标是「用户只写 .py + .xml」。入口只有一种正确写法（开 DPI 感知、
        # 建脚本、跑事件循环、释放），让用户手抄一遍只会多出无数抄错的机会，
        # 而且每加一个示例就多一个几乎相同的文件。生成它以后，源码目录里
        # 一个 .cpp 都不需要。
        #
        # 需要自定义入口（托盘图标、单实例、自己 parse 参数……）的用户，
        # 仍然可以自己写 main.cpp 并链接同一个生成目标——那时 --no-main
        # 关掉这个文件即可，两条路不冲突。
        if not args.no_main:
            write_if_changed(os.path.join(args.out, stem + '.main.gen.cpp'),
                             main_source(class_name, stem, args.windows))

    for source, error in errors:
        print('%s:%d:%d: error: %s' % (source, error.line, error.column, error.message),
              file=sys.stderr)

    if errors:
        print('pyuic: %d 个错误' % len(errors), file=sys.stderr)
        return 1

    if args.header:
        content = (
            '// 由 pyuic 生成，请勿手工修改\n'
            '#pragma once\n\n'
            'namespace pyui {\nnamespace py {\n\n'
            'class Script;\n\n'
            + '\n'.join(header_lines) +
            '\n\n}  // namespace py\n}  // namespace pyui\n')
        write_if_changed(args.header, content)

    # used.json：pyuic 有 AST 和行号，所以「记录」由它负责；
    # 「按清单决定链接什么」由 C++ 的 pyui_extc 负责。两边职责不重叠。
    #
    # 即使没有用到任何扩展也照样写：CMake 的 OUTPUT 列表必须稳定，
    # 缺一个文件会让每次构建都重新 configure。
    if args.used:
        payload = {'modules': []}
        # 顺序按 used_modules 的 import 顺序，便于人读和 diff
        ordered = []
        for module_name in (extension_index.module_names() if extension_index else []):
            if module_name in used_sites:
                ordered.append(module_name)
        for module_name in ordered:
            functions = used_sites[module_name]
            payload['modules'].append({
                'name': module_name,
                'functions': sorted(functions),
                'sites': [
                    {'file': site[0], 'line': site[1], 'column': site[2]}
                    for function_name in sorted(functions)
                    for site in functions[function_name]
                ],
            })
        content = json.dumps(payload, ensure_ascii=False, indent=2) + '\n'
        write_if_changed(args.used, content)

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
