#!/usr/bin/env python3
"""把 Python 脚本编译成 AiPy 虚拟机的字节码。

── 为什么编译器在 AiPy 里，不在宿主里 ──

「怎么编译自己的语言」是语言实现的一半。CPython 的 `compile` 与 `marshal`
都在解释器里，没人把编译器挂在某个 GUI 框架下。放到宿主里会有两个后果：
换个宿主就要重写一份编译器；而 AiPy 被单独拿走时就编不出字节码，等于
「语言能用但没法往里面送程序」。

所以这里只依赖标准库 `ast`，不 import 任何 PyUI 的东西。

── 与 AOT 路线的关系 ──

PyUI 的 `pyuic.py`（在另一个仓库）把同一份脚本编译成 C++，产物里没有
解释器。两条路线**共用同一套语义约束**：Python 语言子集只有一份定义，
不是「翻译目标」各定义一份。差别只在最后一步——那边生成 C++ 表达式，
这边生成指令。

── 为什么必须有这一条路线 ──

AOT 快（300 万次累加 1.11 ms），但改一行脚本要重编译并重新链接整个 exe。
写界面时改一行等十几秒，反馈周期会把人赶走。字节码把这一步压到几十毫秒：
生成字节码 → 加载 → 跑。

代价是慢（同基准 477 ms）。界面脚本的瓶颈在重绘与 GDI，不在算术，这个
代价可以接受。

── 指令编码 ──

必须与 `AiPy/include/aipy/vm.h` 的 `Op` 枚举逐条对齐。这里用名字映射而不是
数字常量：缩进、插新指令都不会让编号悄悄错位，错了立刻在 `check_opcodes()`
里报出来。
"""

import ast
import io
import json
import sys

# ── 操作码（与 AiPy/include/aipy/vm.h 的 Op 一致，顺序即编号）──
#
# 两边各有一份顺序表：这边是名字列表，那边是 enum class。编号错一位就是
# 「所有指令集体错位」，症状是程序算出莫名其妙的结果而不是崩溃。所以
# AiPy/tests/build_bytecode.py 会解析 vm.h 的 Op 枚举逐条对拍，顺序变了
# 立刻在构建期报出来。
OPCODES = [
    'CONST', 'NONE', 'TRUE', 'FALSE',
    'LOAD_LOCAL', 'STORE_LOCAL', 'LOAD_GLOBAL', 'STORE_GLOBAL',
    'BIN', 'CMP', 'UNARY',
    'JUMP', 'JUMP_IF_FALSE', 'JUMP_IF_FALSE_KEEP', 'JUMP_IF_TRUE_KEEP',
    'POP', 'DUP',
    'CALL', 'CALL_BUILTIN', 'CALL_METHOD', 'CALL_EXT',
    'RETURN',
    'BUILD_LIST', 'BUILD_DICT',
    'GET_ITEM', 'SET_ITEM', 'SLICE', 'SET_SLICE',
    'ITER_INIT', 'ITER_NEXT', 'ITER_BREAK',
    'INPLACE',
    'WITH_ENTER', 'WITH_EXIT',
    'SETUP_EXC', 'POP_EXC', 'RAISE', 'RAISE_TYPE', 'RERAISE',
    'EXC_MATCH', 'CURRENT_EXC', 'CLEAR_EXC',
    'DEL_LOCAL', 'DEL_GLOBAL', 'BUILD_TUPLE', 'BUILD_SET',
    # 追加在末尾：Op 的数值就是字节码里的操作码，插到中间会让已生成的
    # .bc 被执行成别的指令。
    'CALL_DYN', 'DICT_MERGE', 'MAKE_FUNCTION', 'CALL_VALUE',
    'CALL_VALUE_DYN', 'MAKE_CLASS', 'GET_ATTR', 'SET_ATTR', 'INSTANTIATE',
    'MAKE_SUPER', 'YIELD', 'AWAIT',
    'MAKE_STATIC_METHOD', 'MAKE_CLASS_METHOD', 'LOAD_TYPE', 'UNPACK',
    'CALL_BUILTIN_KW', 'MAKE_BUILTIN_FUNCTION', 'CALL_METHOD_KW',
    'MAKE_CELL', 'LOAD_CELL', 'STORE_CELL',
]

# 二元算子编号，与 vm.h 的 BinOp 一致。
BINOPS = ['ADD', 'SUB', 'MUL', 'DIV', 'FLOOR_DIV', 'MOD', 'POW',
          'BIT_AND', 'BIT_OR', 'BIT_XOR', 'SHL', 'SHR']

CMPOPS = ['EQ', 'NE', 'LT', 'LE', 'GT', 'GE', 'IN', 'NOT_IN', 'IS', 'IS_NOT']

UNOPS = ['NEG', 'NOT', 'INVERT']

# 内建函数名，与 AiPy/src/builtins.cpp 的 kBuiltinTable 一致。
BUILTINS = [
    'len', 'str', 'int', 'float', 'bool', 'abs', 'round', 'min', 'max',
    'sum', 'list', 'dict', 'range', 'print',
    'format_text', 'char_from_code', 'text_of_char', 'set', 'next',
    'type', 'isinstance',
    'sorted',
    'reversed',
    'zip',
    'enumerate',
    'any',
    'all',
    'chr',
    'ord',
    'hex',
    'bin',
    'oct',
    'repr',
    'divmod',
    'pow',
    'tuple',
    'map',
    'filter',
    'id',
    'hash',
    'help',
    'dir',
]

OP = {name: index for index, name in enumerate(OPCODES)}
BINOP = {name: index for index, name in enumerate(BINOPS)}
CMPOP = {name: index for index, name in enumerate(CMPOPS)}
UNOP = {name: index for index, name in enumerate(UNOPS)}
BUILTIN_INDEX = {name: index for index, name in enumerate(BUILTINS)}

# 内建异常类的名字，与 AiPy/include/aipy/exceptions.h 一致。
#
# 为什么这里是手抄而不是读那个头：Python 版是 oracle，只在测试期跑；
# 读 C++ 头要写一个解析器，而解析别人源码的代码本身会成为新的不一致来源。
# 两边各写一份、由 aipy_compiler_parity 的探针对齐更直接。
# 内建类型名：出现在值位置时编成 LOAD_TYPE。与 include/aipy/types_names.h
# 同内容，靠 probe 对拍保证不分叉。
BUILTIN_TYPE_NAMES = [
    'NoneType', 'bool', 'int', 'float', 'complex',
    'str', 'bytes', 'list', 'tuple', 'dict',
    'set', 'range', 'object', 'type',
]

EXCEPTION_CLASSES = [
    'BaseException', 'Exception', 'SystemExit', 'KeyboardInterrupt',
    'ArithmeticError', 'ZeroDivisionError', 'OverflowError',
    'LookupError', 'IndexError', 'KeyError',
    'ValueError', 'TypeError', 'NameError', 'AttributeError',
    'RuntimeError', 'RecursionError', 'NotImplementedError', 'MemoryError',
    'OSError', 'FileNotFoundError', 'PermissionError', 'FileExistsError',
    'IsADirectoryError', 'UnicodeError', 'EOFError', 'StopIteration',
    'AssertionError',
]
EXCEPTION_SET = set(EXCEPTION_CLASSES)


class CompileError(Exception):
    """带源位置的编译错误。位置格式是 `file:line:col`，与 pyuic 一致。"""

    def __init__(self, message, filename, node):
        line = getattr(node, 'lineno', 0)
        col = getattr(node, 'col_offset', 0) + 1
        super().__init__('%s:%d:%d: error: %s' % (filename, line, col, message))


class Block:
    """一个函数的字节码与它的元信息。"""

    def __init__(self, name, param_count=0):
        self.name = name
        self.code = bytearray()
        self.lines = []          # [(offset, line)]
        self.param_count = param_count
        self.locals = {}         # 名字 → 槽号
        self.local_count = param_count
        # 尾部带默认值的参数个数 + 存放默认值的隐藏全局槽（-1 表示没有）。
        self.default_count = 0
        self.default_slot = -1
        # 每个具名参数一位：置位表示有默认值。仅关键字参数的默认值可以不
        # 连续（`def f(*, a=1, b)`），所以必须有掩码。
        self.default_mask = 0
        # 仅关键字参数的默认值：名字 → 表达式节点。
        self.kwonly_defaults = {}
        # `*args` / `**kwargs` 的局部槽，以及仅关键字参数的起点。
        self.vararg_slot = -1
        self.kwarg_slot = -1
        self.kwonly_start = -1
        # 位置-only 参数个数（`def f(a, b, /, c)` 里是 2）。
        self.posonly_count = 0
        # 槽里存的是「单元」（单元素列表）的名字，收发走 LOAD_CELL /
        # STORE_CELL。父函数里是被内层引用的局部量，子函数里是捕获的自由变量。
        self.cell_names = []
        self.capture_names = []
        self.capture_start = -1
        # 函数体里有 yield：调用它得到生成器，不是返回值。
        self.is_generator = False
        # `async def`：调用它得到协程，await 才驱动它跑。
        self.is_coroutine = False

    def emit(self, op, node):
        self.lines.append((len(self.code), getattr(node, 'lineno', 0)))
        self.code.append(OP[op])

    def emit_u8(self, value):
        self.code.append(value & 0xFF)

    def emit_u16(self, value):
        self.code.append(value & 0xFF)
        self.code.append((value >> 8) & 0xFF)

    def patch_u16(self, at, value):
        self.code[at] = value & 0xFF
        self.code[at + 1] = (value >> 8) & 0xFF

    def param_names(self):
        """具名参数按槽号排列（*args / **kwargs 不算参数）。"""
        names = [None] * self.param_count
        for name, index in self.locals.items():
            if 0 <= index < self.param_count:
                names[index] = name
        return [name for name in names if name is not None]

    def slot_of(self, name):
        if name not in self.locals:
            self.locals[name] = self.local_count
            self.local_count += 1
        return self.locals[name]

    def alloc_slot(self):
        """分配一个匿名局部槽（with 的管理器用），不登记名字。

        只增不减：退出时把槽还回去会让紧随的另一个 with 复用它，而外层的
        `__exit__` 还指着那个槽——管理器被换掉，锁就不释放了。
        """
        slot = self.local_count
        self.local_count += 1
        return slot


class MethodFlattener(ast.NodeTransformer):
    """把方法体里的 `self.x` 改写成模块级名字 `x`。

    为什么用 AST 改写而不是在生成器里特判：`self` 可能出现在任意深度的
    表达式里（`self.a[self.b] + self.c`）。在生成器里每处理一种节点就判一次
    `isinstance(node, ast.Attribute) and node.value.id == 'self'` 是重复劳动，
    而且漏掉一处就是「读到了不存在的变量」。改写一次，后面所有逻辑都不必
    知道 `self` 存在。
    """

    def __init__(self, node, params):
        self.node = node
        self.params = params
        # 通过 `self.x = ...` 赋值的名字。展开后它们与「模块级变量」长得一样，
        # 但意图完全不同：写 `self.x` 就是在改实例状态（这里等同模块状态），
        # 不该被「漏写 global」那道检查拦下来。
        self.self_assigned = set()

    def run(self):
        body = [self.visit(statement) for statement in self.node.body]
        new = ast.FunctionDef(
            name=self.node.name,
            args=ast.arguments(
                posonlyargs=[],
                args=[ast.arg(arg=name) for name in self.params],
                vararg=None,
                kwonlyargs=[],
                kw_defaults=[],
                kwarg=None,
                defaults=[]),
            body=body,
            decorator_list=[],
            returns=None,
            lineno=self.node.lineno,
            col_offset=self.node.col_offset,
        )
        new = ast.fix_missing_locations(new)
        new.aipy_self_assigned = self.self_assigned
        return new

    def _note_target(self, target):
        if isinstance(target, ast.Attribute) and isinstance(target.value, ast.Name) \
                and target.value.id == 'self':
            self.self_assigned.add(target.attr)

    def visit_Assign(self, node):
        for target in node.targets:
            self._note_target(target)
        return self.generic_visit(node)

    def visit_AugAssign(self, node):
        self._note_target(node.target)
        return self.generic_visit(node)

    def visit_AnnAssign(self, node):
        self._note_target(node.target)
        return self.generic_visit(node)

    def visit_Attribute(self, node):
        # self.x → Name('x')
        if isinstance(node.value, ast.Name) and node.value.id == 'self':
            return ast.copy_location(ast.Name(id=node.attr, ctx=node.ctx), node)
        return self.generic_visit(node)


class Compiler:
    """一次编译。只覆盖 SUPPORT.md 的 A 类语法。"""

    def __init__(self, filename, module_name='main'):
        self.filename = filename
        self.module_name = module_name
        self.consts = []
        self.functions = []            # [Function dict]
        self.function_by_name = {}
        # lambda 没有名字，函数表里用 <lambda>N 做唯一标识（只用于报错）。
        self.lambda_counter = 0
        # 嵌套 def 的函数表名（`<外层>.<本层>`），函数体最后统一编译。
        self.nested_function_names = []
        # 带装饰器的 def：名字运行期装的是装饰器返回值，不能再按名解析成
        # 函数号，调用得走按值调用。
        self.decorated_names = set()
        # 普通 Python 类的类名，`A()` 据此发 INSTANTIATE。
        self.class_names = set()
        # 当前正在编译的方法所属的类；`super()` 需要它。
        self.current_class = ''
        # 等待编译函数体的 lambda 下标：登记发生在编译某个函数体/模块语句
        # 的中途，函数体要等所有全局槽就位后再编。
        self.pending_lambdas = []
        self.globals = {}              # 模块级变量名 → 全局槽
        self.assigned_so_far = set()   # 已按源码顺序编译过的模块级赋值
        self.global_count = 0
        self.module = Block('<module>')
        self.block = self.module
        self.loop_stack = []           # [(continue_target, [break_fixups], is_for)]
        # 当前嵌套的 with 语句：[(临时槽号, ...)]。退出路径（return / break /
        # continue）要按**由内向外**的顺序给每一层插一条 WITH_EXIT，否则锁
        # 不会释放。用栈而不是单个槽号：with 可以嵌套任意层。
        self.with_stack = []
        self.imported = []             # 扩展模块名（`import text` 里的 text）
        self.extensions = []           # 用到的「模块.函数」限定名，顺序即 CALL_EXT 下标
        self.used_builtins = []        # 用到过的内建名，顺序即 CALL_BUILTIN 下标

    def extension_index(self, qualified):
        """按首次使用顺序发扩展下标。

        与内建同一个理由：一个应用通常只碰几个扩展函数，没必要让产物里
        出现整份模块的函数表。下标只对**这个产物**有意义，加载器按名字
        查指针，所以换个脚本、换个链接组合都不会错位。
        """
        if qualified not in self.extensions:
            self.extensions.append(qualified)
        return self.extensions.index(qualified)

    def dotted_name(self, node):
        """把纯属性链摊成点分名（`os` / `os.path` / `os.path.joiner`）。

        只接受「根是 Name、往上全是 Attribute」这一种形态：`f().x` 或
        `a[0].x` 不是模块引用，返回空串让调用方走方法调用那条路。
        """
        if isinstance(node, ast.Name):
            return node.id
        if isinstance(node, ast.Attribute):
            base = self.dotted_name(node.value)
            return base + '.' + node.attr if base else ''
        return ''

    def is_module(self, name):
        """这个点分名是不是 import 过的模块。

        两条判据：
          1. 精确匹配（`import os.path`）
          2. 点分边界上的前缀匹配（`import os` 之后写 `os.path.exists`）
        第 2 条是 CPython 的真实行为：`import os` 之后 `os.path` 就可用。
        """
        for module in self.imported:
            if name == module:
                return True
            if name.startswith(module + '.'):
                return True
        return False

    def builtin_index(self, name):
        """按首次使用顺序发内建下标。

        为什么不直接用 BUILTINS 的全局下标：那个表有 18 项，而
        一个脚本通常只用两三个。只把用到的写进产物，加载器就不必认识全部
        内建，版本差异（新运行时少了某个内建）也只影响真正用到它的脚本。
        """
        if name not in self.used_builtins:
            self.used_builtins.append(name)
        return self.used_builtins.index(name)

    # ── 常量池 ──

    def const_index(self, value):
        for index, existing in enumerate(self.consts):
            if type(existing) is type(value) and existing == value:
                return index
        self.consts.append(value)
        return len(self.consts) - 1

    def const_string(self, text):
        return self.const_index(text)

    # ── 全局槽 ──

    def global_slot(self, name):
        if name not in self.globals:
            self.globals[name] = self.global_count
            self.global_count += 1
        return self.globals[name]

    # ── 作用域 ──

    def is_local(self, name):
        if self.block is self.module:
            return False
        return name in self.block.locals

    def name_target(self, name, node):
        """写出一个名字：单元优先，其次局部，否则当全局。"""
        if self.loads_cell(name):
            self.block.emit('LOAD_CELL', node)
            self.block.emit_u16(self.block.slot_of(name))
            return
        if self.is_local(name):
            self.block.emit('LOAD_LOCAL', node)
            self.block.emit_u16(self.block.slot_of(name))
        else:
            self.block.emit('LOAD_GLOBAL', node)
            self.block.emit_u16(self.global_slot(name))

    def name_store(self, name, node):
        if self.loads_cell(name):
            self.block.emit('STORE_CELL', node)
            self.block.emit_u16(self.block.slot_of(name))
            return
        if self.is_local(name):
            self.block.emit('STORE_LOCAL', node)
            self.block.emit_u16(self.block.slot_of(name))
        else:
            self.block.emit('STORE_GLOBAL', node)
            self.block.emit_u16(self.global_slot(name))
            # 模块级赋值一旦编译过，后续读同名名字就是读变量而非内建。
            # 不能用预扫描的 self.globals：`int = 5` 写在后面时前面的
            # `int` 在 CPython 里仍是内建。
            if self.block is self.module:
                self.assigned_so_far.add(name)

    # ── 入口 ──

    def compile_module(self, tree):
        body = list(tree.body)
        # Activity 类：脚本的入口形式。类体赋值是字段（→ 全局槽），
        # 类里的方法编译成普通函数，`self` 参数被去掉——一个脚本只有一个
        # 实例，`self` 与「模块状态」是同一件事。
        #
        # 判断依据是「基类里写了 Activity」。普通 Python 类（`class A:`）
        # 走另一条路（MAKE_CLASS），不能被这条吞掉。
        activity = None
        for node in body:
            if not isinstance(node, ast.ClassDef):
                continue
            base_names = [b.id for b in node.bases if isinstance(b, ast.Name)]
            if 'Activity' not in base_names:
                continue
            if activity is not None:
                raise CompileError('只能有一个 Activity 类', self.filename, node)
            activity = node

        # Activity 之外的 class 都是普通 Python 类。
        plain_classes = [node for node in body
                         if isinstance(node, ast.ClassDef) and node is not activity]
        for node in plain_classes:
            self.class_names.add(node.name)

        if activity is not None:
            body = self.expand_activity(activity, body)

        # 类方法登记在模块级之前：`A()` 与 `class B(A)` 都要能认出它们。
        for node in plain_classes:
            self.register_class_methods(node)

        # 先把所有顶层 def 收进来：Python 允许「先调用后定义」，而模块级
        # 代码按顺序执行，所以函数必须先全部就位。
        for node in body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                self.register_function(node)

        # 嵌套 def：登记必须在**父函数的局部量预扫描之后**，否则算不出
        # 「哪些外层名字被内层引用」。
        for node in body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                index = self.function_by_name[node.name]
                self.register_nested_functions(
                    node, node.name, self.functions[index]['block'])

        # 模块级变量先全部占好槽：函数里读模块级变量要能找到它。
        for node in body:
            for name in self.assigned_names(node):
                self.global_slot(name)

        # import 语句先全部登记：函数体里 `threading.current_thread()` 要靠
        # `self.imported` 才认得出「这是扩展调用而不是方法调用」。不预扫的
        # 话，编译函数体时 imported 还是空的，函数体内的扩展调用会被当成
        # 普通方法调用——症状是「模块级能调、函数里静默失败」。
        #
        # import 本身不产生字节码（依赖图是构建期的事），所以这里只是把
        # 「哪些模块被用到」提前记下来，后面模块级执行时不会再重复记录
        # （import_statement 对同一模块是幂等的）。
        for node in ast.walk(tree):
            if isinstance(node, (ast.Import, ast.ImportFrom)):
                self.import_statement(node)

        # 函数体最后编译：这时所有函数名与全局槽都已就位，才能支持
        # 「前面的方法调用后面的方法」，也才能检查出「函数里偷偷改模块级
        # 变量却没写 global」。
        for node in body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                self.compile_function_body(node)

        # 嵌套 def 的函数体：名字是 `<外层>.<本层>`，显式传进去。
        for name in self.nested_function_names:
            entry = self.functions[self.function_by_name[name]]
            self.compile_function_body(entry['node'], name)

        # 类的方法体也要编。
        for node in plain_classes:
            for member in node.body:
                if isinstance(member, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    self.compile_function_body(
                        member, node.name + '.' + member.name)

        # 默认值在**定义处**求值（CPython 语义），所以它在 statement() 处理
        # FunctionDef 时发射，位置就在这里不是别处。

        # 模块级语句：按源码顺序执行。放在函数编译之后，这样
        # `g = f`（读函数名）与 `f = 3`（覆盖函数）都能被拒绝。
        for node in body:
            self.statement(node)

        # lambda 的函数体在模块级语句编完之后才编：登记发生在语句内部。
        self.compile_pending_lambdas()

        # 模块级最后一条语句结束 → 隐式 return None
        self.module.emit('NONE', tree)
        self.module.emit('RETURN', tree)
        return self.build_program()

    def emit_default_values(self, node):
        """把默认参数值在定义处求值，存进函数自己的隐藏全局槽。

        列表与 paramCount 等长，按参数下标放；没有默认值的位置留占位
        （函数表里的掩码说明哪些位置是真的）。这样仅关键字参数的默认值
        可以不在尾部（`def f(*, a=1, b)`）。
        """
        entry = self.functions[self.function_by_name[node.name]]
        block = entry['block']
        args = node.args
        is_method = bool(args.args) and args.args[0].arg == 'self'
        first = 1 if is_method else 0

        for _ in range(block.param_count):
            self.module.emit('NONE', node)
        self.module.emit('BUILD_LIST', node)
        self.module.emit_u16(block.param_count)

        # 位置参数的默认值只能出现在尾部：defaults[k] 对应第
        # (len(args.args) - len(defaults) + k) 个参数，去掉 self 后减 first。
        base = len(args.args) - len(args.defaults) - first
        for offset, default in enumerate(args.defaults):
            self.module.emit('DUP', node)
            self.module.emit('CONST', node)
            self.module.emit_u16(self.const_index(base + offset))
            self.expr(default)
            self.module.emit('SET_ITEM', node)
        for offset, kwarg in enumerate(args.kwonlyargs):
            if kwarg.arg not in block.kwonly_defaults:
                continue
            self.module.emit('DUP', node)
            self.module.emit('CONST', node)
            self.module.emit_u16(self.const_index(
                block.kwonly_start + offset))
            self.expr(block.kwonly_defaults[kwarg.arg])
            self.module.emit('SET_ITEM', node)

        self.module.emit('STORE_GLOBAL', node)
        self.module.emit_u16(block.default_slot)

    def expand_activity(self, node, body):
        """把 `class X(Activity)` 展开成「模块级赋值 + 顶层函数」。

        为什么这样展开：字节码路线没有对象模型（SUPPORT.md C 类）。但
        Activity 不是对象模型——它只是「一组字段 + 一组方法」的容器，一个
        脚本只有一个实例。展开之后 `self.entry` 就是模块级变量 `entry`，
        `self.show()` 就是函数 `show()`，语义完全对应，不需要 vm 里多一类
        「对象」。
        """
        if not any(isinstance(base, ast.Name) and base.id == 'Activity'
                   for base in node.bases):
            raise CompileError('类必须继承 Activity', self.filename, node)
        if node.decorator_list:
            raise CompileError('不支持装饰器', self.filename, node.decorator_list[0])

        expanded = []
        # 类体里的赋值 → 模块级赋值
        for statement in node.body:
            if isinstance(statement, (ast.Assign, ast.AnnAssign)):
                expanded.append(statement)
            elif isinstance(statement, ast.FunctionDef):
                continue   # 下面统一加
            elif isinstance(statement, ast.Pass):
                continue
            else:
                raise CompileError(
                    'Activity 类体里只允许字段赋值和方法定义', self.filename, statement)

        # 方法 → 顶层函数（去掉 self 参数，self.xxx → xxx）
        for statement in node.body:
            if isinstance(statement, ast.FunctionDef):
                expanded.append(self.flatten_method(statement))

        # 类之外的顶层语句保留（模块级常量、辅助函数）
        for statement in body:
            if statement is not node:
                expanded.append(statement)
        return expanded

    def flatten_method(self, node):
        """把方法转成顶层函数：去掉 self，`self.x` 改写为 `x`。"""
        args = node.args
        params = [arg.arg for arg in args.posonlyargs] + [arg.arg for arg in args.args]
        if not params or params[0] != 'self':
            raise CompileError('方法 "%s" 的第一个参数必须是 self' % node.name,
                               self.filename, node)
        return MethodFlattener(node, params[1:]).run()

    def assigned_names(self, node):
        """模块级语句里被赋值的名字。只处理 SUPPORT.md 允许的形式。"""
        names = []
        if isinstance(node, ast.Assign):
            for target in node.targets:
                names.extend(self.target_names(target))
        elif isinstance(node, ast.AnnAssign) and node.value is not None:
            names.extend(self.target_names(node.target))
        elif isinstance(node, ast.AugAssign):
            names.extend(self.target_names(node.target))
        return names

    def target_names(self, target):
        if isinstance(target, ast.Name):
            return [target.id]
        if isinstance(target, (ast.Tuple, ast.List)):
            names = []
            for element in target.elts:
                names.extend(self.target_names(element))
            return names
        return []

    # ── 函数 ──

    def register_function(self, node, name=None, keep_self=False):
        """只登记签名与槽位，不编译函数体。

        分两步的理由：编译函数体时会解析「调用哪个函数」，而 Python 允许
        「前面的方法调用后面的方法」。先登记全部签名，编译时才能查到。

        `name` 给类方法用（`<类>.<方法>`，避免与模块级函数撞名）；
        `keep_self` 表示保留 self 形参——Activity 的方法要剥掉它（一个
        脚本只有一个实例），普通类的方法必须留着。
        """
        if name is None:
            name = node.name
        if name in self.function_by_name:
            raise CompileError('函数 "%s" 定义了两次' % name, self.filename, node)
        if node.decorator_list and name == node.name:
            # 带装饰器的名字运行期装的是装饰器返回值，按名调用要走按值调用。
            self.decorated_names.add(name)

        args = node.args
        # 位置-only 参数（`def f(a, /)`）按普通位置参数登记；按关键字传它
        # 时，VM 的 bindArguments 按名字匹配不到就报 got an unexpected
        # keyword argument，与 CPython 同形。
        params = [arg.arg for arg in args.posonlyargs]
        params += [arg.arg for arg in args.args]
        posonly_count = len(args.posonlyargs)
        kwonly = [arg.arg for arg in args.kwonlyargs]
        is_method = not keep_self and bool(args.args) and args.args[0].arg == 'self'
        first = 1 if is_method else 0

        block = Block(name, len(params) - first)
        for index in range(first, len(params)):
            block.locals[params[index]] = index - first
        block.local_count = block.param_count
        # 位置-only 参数排在参数表最前；减掉被方法剥掉的 self。
        block.posonly_count = max(0, posonly_count - first)
        if kwonly:
            block.kwonly_start = block.param_count
            for offset, parameter in enumerate(kwonly):
                block.locals[parameter] = block.param_count
                block.param_count += 1
            block.local_count = block.param_count
        if args.vararg is not None:
            block.vararg_slot = block.local_count
            block.locals[args.vararg.arg] = block.local_count
            block.local_count += 1
        if args.kwarg is not None:
            block.kwarg_slot = block.local_count
            block.locals[args.kwarg.arg] = block.local_count
            block.local_count += 1

        # 默认值：每个具名参数一位。位置参数的默认值只能出现在尾部，所以
        # 它们从 len(params) - len(defaults) 个参数开始（再去掉 self）。
        mask = 0
        default_base = len(params) - len(args.defaults) - first
        for offset, _default in enumerate(args.defaults):
            mask |= 1 << (default_base + offset)
        kwonly_defaults = {}
        for offset, kwarg in enumerate(args.kwonlyargs):
            default = args.kw_defaults[offset] if args.kw_defaults else None
            if default is not None:
                kwonly_defaults[kwarg.arg] = default
                mask |= 1 << (block.kwonly_start + offset)
        block.default_mask = mask
        block.kwonly_defaults = kwonly_defaults
        if args.defaults or kwonly_defaults:
            block.default_count = bin(mask).count('1')
            # 默认值在定义处求值，存进一个隐藏全局槽（与 C++ 版同一形状）。
            block.default_slot = self.global_slot('\x01defaults:' + node.name)

        # 预扫描函数体里「会被赋值」的名字，先占好局部槽。
        #
        # 不预扫的后果不是「性能差」而是**语义错**：`name_store` 先问
        # `is_local`，而局部量要到第一次赋值时由 `slot_of` 才登记——那一刻
        # 已经判成全局，于是 `x = 0` 写成模块级变量，函数内的临时量变成
        # 进程共享的全局槽。单线程下看起来正常（同名变量只被一个函数用），
        # 多线程或递归立刻互相踩踏：探针里 `def fib` 的 left/right/total
        # 全进了 globals，四个线程共享 subtotal 就把和算成 3865。
        #
        # 声明了 `global X` 的名字不算局部——那是 Python 的作用域规则本身。
        # `self.x = ...` 展开来的名字也不算：它们是 Activity 的字段（模块级
        # 状态），被 MethodFlattener 改写成与模块变量同名，但意图是改实例。
        declared_global = set()
        declared_nonlocal = set()
        # 只扫本函数的语句，**不钻进内层 def**：内层的 global / nonlocal
        # 与赋值都是它自己的作用域，算到父函数上会让父块的局部量判错。
        for child in self._scope_nodes(node):
            if isinstance(child, ast.Global):
                declared_global.update(child.names)
            elif isinstance(child, ast.Nonlocal):
                declared_nonlocal.update(child.names)
        self_assigned = getattr(node, 'aipy_self_assigned', set())

        # 记下「预扫描登记的局部名」，供 global 检查区分「本就是局部」与
        # 「因为预扫描才进了 locals」：后者若同时是模块级变量，仍然是漏写
        # global 的笔误，该拒绝。
        preassigned = set()
        for child in self._scope_nodes(node):
            if not isinstance(child, (ast.Assign, ast.AugAssign, ast.AnnAssign)):
                continue
            targets = child.targets if isinstance(child, ast.Assign) else [child.target]
            for target in targets:
                for target_name in self.target_names(target):
                    if target_name in declared_global or target_name in self_assigned:
                        continue
                    # `nonlocal x` 的 x 是外层的变量，不是本函数的局部量。
                    if target_name in declared_nonlocal:
                        continue
                    block.slot_of(target_name)
                    preassigned.add(target_name)
        block.preassigned = preassigned

        index = len(self.functions)
        self.function_by_name[name] = index
        self.functions.append({'block': block, 'node': node})


    # ── 嵌套函数 ──

    def register_nested_functions(self, fn_node, qualified, parent):
        pending_nested = []
        self._register_nested_pass(fn_node, qualified, parent, pending_nested)
        for child, block, owner in pending_nested:
            free = self.free_names(child, block, owner)
            block.capture_names = free
            if free:
                block.capture_start = block.local_count
                for captured in free:
                    block.slot_of(captured)
            for captured in free:
                if captured not in owner.cell_names:
                    owner.cell_names.append(captured)

    def _register_nested_pass(self, fn_node, qualified, parent, pending_nested):
        for child in fn_node.body:
            if not isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            name = qualified + '.' + child.name
            if name in self.function_by_name:
                raise CompileError('函数 "%s" 定义了两次' % name,
                                   self.filename, child)
            self.register_function(child, name)
            self.nested_function_names.append(name)
            block = self.functions[self.function_by_name[name]]['block']
            pending_nested.append((child, block, parent))
            self._register_nested_pass(child, name, block, pending_nested)

    def _scope_nodes(self, fn_node):
        """遍历本函数作用域内的节点：进入语句的嵌套体，但不进内层 def。"""
        queue = list(fn_node.body)
        while queue:
            current = queue.pop(0)
            yield current
            if self._is_fn(current):
                continue
            queue.extend(ast.iter_child_nodes(current))

    @staticmethod
    def _is_fn(node):
        return isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))

    def _collect_nonlocals(self, fn_node):
        """函数体里声明的 nonlocal 名字（不钻进更内层的函数）。"""
        out = set()
        queue = list(fn_node.body)
        while queue:
            current = queue.pop(0)
            if isinstance(current, ast.Nonlocal):
                out.update(current.names)
            if self._is_fn(current):
                continue
            queue.extend(ast.iter_child_nodes(current))
        return out

    def _collect_reads(self, node, out):
        """读到的名字（不钻进内层函数体 / lambda 体；def 的默认值与
        装饰器在外层求值，要算进来）。"""
        queue = [node]
        while queue:
            current = queue.pop(0)
            if isinstance(current, ast.Name):
                if current.id not in out:
                    out.append(current.id)
                continue
            if isinstance(current, ast.Lambda):
                continue
            if self._is_fn(current):
                args = current.args
                for item in list(args.defaults) + [
                        d for d in args.kw_defaults if d is not None]:
                    queue.append(item)
                queue.extend(current.decorator_list)
                continue
            queue.extend(ast.iter_child_nodes(current))

    def free_names(self, body, self_block, parent):
        """body 要跨作用域读取的名字，顺序与 C++ 版一致。"""
        out = []
        for name in sorted(self._collect_nonlocals(body)):
            if name in parent.locals and name not in out:
                out.append(name)
        reads = []
        for statement in body.body:
            self._collect_reads(statement, reads)
        for name in reads:
            if name in BUILTIN_INDEX:
                continue
            if name in self.function_by_name:
                continue
            if name in self_block.locals:
                continue
            if name not in parent.locals:
                continue
            if name not in out:
                out.append(name)
        return out

    def loads_cell(self, name):
        if self.block is self.module:
            return False
        return name in self.block.cell_names or name in self.block.capture_names

    def register_class_methods(self, node):
        """类里的方法登记成 `<类>.<方法>`，保留 self 形参。"""
        for member in node.body:
            if not isinstance(member, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            self.register_function(member, node.name + '.' + member.name,
                                   keep_self=True)

    def apply_member_descriptors(self, member):
        """类成员的装饰器：栈顶已有一个函数值，就地应用。

        与模块级 apply_decorators 的差别是值不从全局槽取——类体里的成员值
        是 MAKE_CLASS 从栈上收的。@staticmethod / @classmethod 在编译期
        识别：它们是「访问时怎么绑定」的标记，必须变成包装值进成员表。
        """
        if not member.decorator_list:
            return

        for decorator in reversed(member.decorator_list):
            if (isinstance(decorator, ast.Name) and
                    decorator.id in ('staticmethod', 'classmethod')):
                self.module.emit(
                    'MAKE_STATIC_METHOD' if decorator.id == 'staticmethod'
                    else 'MAKE_CLASS_METHOD', decorator)
                continue
            self.expr(decorator)
            value_slot = self.module.slot_of('\x01member_value')
            deco_slot = self.module.slot_of('\x01member_decorator')
            self.module.emit('STORE_LOCAL', member)
            self.module.emit_u16(deco_slot)
            self.module.emit('STORE_LOCAL', member)
            self.module.emit_u16(value_slot)
            self.module.emit('LOAD_LOCAL', member)
            self.module.emit_u16(deco_slot)
            self.module.emit('LOAD_LOCAL', member)
            self.module.emit_u16(value_slot)
            self.module.emit('CALL_VALUE', member)
            self.module.emit_u8(1)

    def compile_class(self, node):
        """类体摊平成「名字 → 值」的成员表，发 MAKE_CLASS。"""
        for base in node.bases:
            self.expr(base)

        member_names = []
        for member in node.body:
            if isinstance(member, (ast.FunctionDef, ast.AsyncFunctionDef)):
                qualified = node.name + '.' + member.name
                member_names.append(member.name)
                self.module.emit('MAKE_FUNCTION', member)
                self.module.emit_u16(self.function_by_name[qualified])
                self.module.emit_u16(0)
                self.apply_member_descriptors(member)
                continue
            if isinstance(member, ast.Assign) and len(member.targets) == 1:
                target = member.targets[0]
                if not isinstance(target, ast.Name):
                    raise CompileError('不支持这种类属性目标', self.filename, member)
                member_names.append(target.id)
                self.expr(member.value)
                continue
            if isinstance(member, ast.AnnAssign):
                # `x: int = 1` 等价于 `x = 1`；纯注解 `x: int` 在类体里
                # 只留下 __annotations__ 记录，没有值，所以不产生成员。
                if not isinstance(member.target, ast.Name):
                    raise CompileError('不支持这种类属性目标', self.filename, member)
                if member.value is None:
                    continue
                member_names.append(member.target.id)
                self.expr(member.value)
                continue
            if isinstance(member, ast.Pass):
                continue
            if isinstance(member, ast.Expr):
                continue  # 文档字符串
            raise CompileError('不支持这种类体语句 %s' % type(member).__name__,
                               self.filename, member)

        self.module.emit('MAKE_CLASS', node)
        self.module.emit_u16(self.const_string(node.name))
        self.module.emit_u16(len(member_names))
        self.module.emit_u16(len(node.bases))
        for name in member_names:
            self.module.emit_u16(self.const_string(name))

        self.name_store(node.name, node)

    def register_lambda(self, node):
        """lambda 也登记成函数表里的一项，只是没有名字。

        与 register_function 的槽位规则逐条对齐，否则字节码对不上。
        """
        name = '<lambda>%d' % self.lambda_counter
        self.lambda_counter += 1

        args = node.args
        params = [arg.arg for arg in args.args]
        kwonly = [arg.arg for arg in args.kwonlyargs]

        block = Block(name, len(params))
        for index, parameter in enumerate(params):
            block.locals[parameter] = index
        block.local_count = block.param_count
        if kwonly:
            block.kwonly_start = block.param_count
            for _offset, parameter in enumerate(kwonly):
                block.locals[parameter] = block.param_count
                block.param_count += 1
            block.local_count = block.param_count
        if args.vararg is not None:
            block.vararg_slot = block.local_count
            block.locals[args.vararg.arg] = block.local_count
            block.local_count += 1
        if args.kwarg is not None:
            block.kwarg_slot = block.local_count
            block.locals[args.kwarg.arg] = block.local_count
            block.local_count += 1

        mask = 0
        base = len(params) - len(args.defaults)
        for offset, _default in enumerate(args.defaults):
            mask |= 1 << (base + offset)
        kwonly_defaults = {}
        for offset, kwarg in enumerate(args.kwonlyargs):
            default = args.kw_defaults[offset] if args.kw_defaults else None
            if default is not None:
                kwonly_defaults[kwarg.arg] = default
                mask |= 1 << (block.kwonly_start + offset)
        block.default_mask = mask
        block.kwonly_defaults = kwonly_defaults
        if mask:
            block.default_count = bin(mask).count('1')
            block.default_slot = self.global_slot('\x01defaults:' + name)

        captures = self.lambda_captures(node.body, params)
        block.captures = captures
        if captures:
            block.capture_start = block.local_count
            for captured in captures:
                block.slot_of(captured)

        index = len(self.functions)
        self.functions.append({'block': block, 'node': node, 'is_lambda': True})
        self.pending_lambdas.append(index)
        return index

    def lambda_captures(self, body, params):
        """lambda 体里引用的外层名字，顺序与 C++ 版的 BFS 一致。"""
        out = []
        queue = [body]
        while queue:
            current = queue.pop(0)
            if isinstance(current, ast.Name):
                name = current.id
                if name in BUILTIN_INDEX:
                    pass
                elif name in self.function_by_name:
                    pass
                elif name in params:
                    pass
                elif name not in out:
                    out.append(name)
                continue
            queue.extend(ast.iter_child_nodes(current))
        return out

    def emit_lambda_defaults(self, node, block):
        """lambda 的默认值在定义处求值，与 def 同构。"""
        args = node.args
        for _ in range(block.param_count):
            self.module.emit('NONE', node)
        self.module.emit('BUILD_LIST', node)
        self.module.emit_u16(block.param_count)
        base = len(args.args) - len(args.defaults)
        for offset, default in enumerate(args.defaults):
            self.module.emit('DUP', node)
            self.module.emit('CONST', node)
            self.module.emit_u16(self.const_index(base + offset))
            self.expr(default)
            self.module.emit('SET_ITEM', node)
        for offset, kwarg in enumerate(args.kwonlyargs):
            if kwarg.arg not in block.kwonly_defaults:
                continue
            self.module.emit('DUP', node)
            self.module.emit('CONST', node)
            self.module.emit_u16(self.const_index(
                block.kwonly_start + offset))
            self.expr(block.kwonly_defaults[kwarg.arg])
            self.module.emit('SET_ITEM', node)
        self.module.emit('STORE_GLOBAL', node)
        self.module.emit_u16(block.default_slot)

    def compile_pending_lambdas(self):
        """lambda 的函数体就是一个表达式，编完直接 RETURN。"""
        for index in self.pending_lambdas:
            entry = self.functions[index]
            block = entry['block']
            node = entry['node']
            saved = self.block
            saved_loops = self.loop_stack
            saved_withs = self.with_stack
            self.block = block
            self.loop_stack = []
            self.with_stack = []
            self.expr(node.body)
            block.emit('RETURN', node)
            self.with_stack = saved_withs
            self.loop_stack = saved_loops
            self.block = saved

    def nested_def_statement(self, node):
        """嵌套 def 的定义处：求值默认值 → 压捕获 → MAKE_FUNCTION →
        应用装饰器 → 存进名字。"""
        qualified = self.block.name + '.' + node.name
        index = self.function_by_name.get(qualified)
        if index is None:
            raise CompileError('函数 "%s" 没有登记' % qualified,
                               self.filename, node)
        block = self.functions[index]['block']

        if block.default_slot >= 0:
            self.emit_default_values_for(node, index)

        self.emit_function_value(index, node)
        self.name_store(node.name, node)

        for decorator in reversed(node.decorator_list):
            self.expr(decorator)
            # [函数值, 装饰器] → [装饰器, 函数值]
            value_slot = self.block.alloc_slot()
            deco_slot = self.block.alloc_slot()
            self.block.emit('STORE_LOCAL', node)
            self.block.emit_u16(deco_slot)
            self.block.emit('STORE_LOCAL', node)
            self.block.emit_u16(value_slot)
            self.block.emit('LOAD_LOCAL', node)
            self.block.emit_u16(deco_slot)
            self.block.emit('LOAD_LOCAL', node)
            self.block.emit_u16(value_slot)
            self.block.emit('CALL_VALUE', node)
            self.block.emit_u8(1)
        if node.decorator_list:
            self.name_store(node.name, node)

    def emit_function_value(self, index, node):
        """先压捕获（单元对象），再 MAKE_FUNCTION。"""
        block = self.functions[index]['block']
        for captured in block.capture_names:
            if self.block is not self.module and self.loads_cell(captured):
                self.block.emit('LOAD_LOCAL', node)
                self.block.emit_u16(self.block.slot_of(captured))
            else:
                self.name_target(captured, node)
                # 值捕获：外面不是单元，包一层单元素列表保持一致。
                self.block.emit('BUILD_LIST', node)
                self.block.emit_u16(1)
        self.block.emit('MAKE_FUNCTION', node)
        self.block.emit_u16(index)
        self.block.emit_u16(len(block.capture_names))

    def emit_default_values_for(self, node, index):
        """按函数号求值默认参数（嵌套 def 用）。"""
        block = self.functions[index]['block']
        args = node.args
        is_method = bool(args.args) and args.args[0].arg == 'self'
        first = 1 if is_method else 0

        for _ in range(block.param_count):
            self.block.emit('NONE', node)
        self.block.emit('BUILD_LIST', node)
        self.block.emit_u16(block.param_count)

        base = len(args.args) - len(args.defaults) - first
        for offset, default in enumerate(args.defaults):
            self.block.emit('DUP', node)
            self.block.emit('CONST', node)
            self.block.emit_u16(self.const_index(base + offset))
            self.expr(default)
            self.block.emit('SET_ITEM', node)
        for offset, kwarg in enumerate(args.kwonlyargs):
            if kwarg.arg not in block.kwonly_defaults:
                continue
            self.block.emit('DUP', node)
            self.block.emit('CONST', node)
            self.block.emit_u16(self.const_index(
                block.kwonly_start + offset))
            self.expr(block.kwonly_defaults[kwarg.arg])
            self.block.emit('SET_ITEM', node)
        self.block.emit('STORE_GLOBAL', node)
        self.block.emit_u16(block.default_slot)

    def apply_decorators(self, node):
        """`@a @b def f` 等价于 `f = a(b(f))`，最靠近 def 的先应用。"""
        index = self.function_by_name[node.name]
        slot = self.global_slot('\x01decorated:' + node.name)

        self.module.emit('MAKE_FUNCTION', node)
        self.module.emit_u16(index)
        self.module.emit_u16(0)
        self.module.emit('STORE_GLOBAL', node)
        self.module.emit_u16(slot)

        for decorator in reversed(node.decorator_list):
            self.expr(decorator)
            self.module.emit('LOAD_GLOBAL', node)
            self.module.emit_u16(slot)
            self.module.emit('CALL_VALUE', node)
            self.module.emit_u8(1)
            self.module.emit('STORE_GLOBAL', node)
            self.module.emit_u16(slot)

        self.module.emit('LOAD_GLOBAL', node)
        self.module.emit_u16(slot)
        self.name_store(node.name, node)

    def compile_function_body(self, node, qualified_name=None):
        """编译已经登记过的函数体。

        必须等**所有**函数登记完、**所有**全局槽占完再调用：前者让「前向调用」
        成立，后者让 global 检查能看见模块级变量。
        """
        name = qualified_name if qualified_name is not None else node.name
        entry = self.functions[self.function_by_name[name]]
        block = entry['block']

        # 函数里对模块级的赋值必须写 global：否则按 CPython 就是新建局部量，
        # 而那几乎总是笔误。与 pyuic 的 check_scope 同一条规则。
        #
        # 例外是通过 `self.x = ...` 来的名字：展开后它们与模块级变量同名，
        # 但原本写的是「改实例状态」，那是 Activity 的正当用法。
        self_assigned = getattr(node, 'aipy_self_assigned', set())
        declared_global = set()
        declared_nonlocal = set()
        for child in ast.walk(node):
            if isinstance(child, ast.Global):
                declared_global.update(child.names)
            elif isinstance(child, ast.Nonlocal):
                declared_nonlocal.update(child.names)

        # 参数占前 param_count 个槽，它们是真正的局部量。
        params = set()
        for param_name, slot in block.locals.items():
            if slot < block.param_count:
                params.add(param_name)

        for child in ast.walk(node):
            if isinstance(child, (ast.Assign, ast.AugAssign, ast.AnnAssign)):
                targets = child.targets if isinstance(child, ast.Assign) else [child.target]
                for target in targets:
                    for assigned_name in self.target_names(target):
                        if assigned_name in self_assigned:
                            continue
                        # `name in block.locals` 不能直接用来豁免：预扫描会
                        # 把所有赋值目标都登记成局部，那样漏写 global 的笔误
                        # 就再也查不出来了。参数才是真正的局部量。
                        if assigned_name in declared_nonlocal:
                            continue
                        if assigned_name in self.globals and assigned_name not in declared_global \
                                and assigned_name not in params:
                            raise CompileError(
                                '函数里给模块级变量 %s 赋值，但没有写 global 声明。'
                                'CPython 会把它当成新的局部变量；要改模块级变量请先写 '
                                '`global %s`' % (assigned_name, assigned_name),
                                self.filename, child)

        # 进入函数上下文编译函数体
        saved = self.block
        saved_loops = self.loop_stack
        saved_withs = self.with_stack
        saved_class = self.current_class
        dot = name.find('.')
        self.current_class = '' if dot < 0 else name[:dot]
        # `async def` 是协程：调用它只造对象，await 才驱动。
        if isinstance(node, ast.AsyncFunctionDef):
            block.is_coroutine = True

        # 函数体里有 yield 就是生成器（lambda 体与内层 def 不算外层的：
        # 它们各自决定）。
        pending = list(node.body)
        while pending and not block.is_generator:
            current = pending.pop()
            if isinstance(current, ast.Lambda):
                continue
            if self._is_fn(current):
                continue
            if isinstance(current, (ast.Yield, ast.YieldFrom)):
                block.is_generator = True
                break
            pending.extend(ast.iter_child_nodes(current))
        self.block = block
        self.loop_stack = []
        self.with_stack = []
        # 闭包单元：本函数里被内层引用的局部量先包成 `[值]`。
        for cell_name in block.cell_names:
            block.emit('MAKE_CELL', node)
            block.emit_u16(block.slot_of(cell_name))
        for statement in node.body:
            self.statement(statement)
        self.with_stack = saved_withs
        self.loop_stack = saved_loops
        self.block = saved
        self.current_class = saved_class

        # 隐式 return None（并且保证 return 落点正确）
        block.emit('NONE', node)
        block.emit('RETURN', node)

    # ── 语句 ──

    def statement(self, node):
        if isinstance(node, ast.Expr):
            if isinstance(node.value, ast.YieldFrom):
                raise CompileError('不支持 yield from', self.filename, node.value)
            if isinstance(node.value, ast.Yield):
                # `yield x` 作为语句：yield 的值没人要，直接丢掉。
                self.expr(node.value)
                self.block.emit('POP', node)
                return
            if isinstance(node.value, ast.Await):
                # `await f()` 作为语句：驱动协程，丢掉返回值。
                self.expr(node.value)
                self.block.emit('POP', node)
                return
            if isinstance(node.value, ast.Call):
                self.expr(node.value)
                self.block.emit('POP', node)
            return

        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            # 顶层 def：签名已在 register_function 登记；这里只发默认值求值，
            # 因为 CPython 在执行到这一句时求值默认参数。
            if self.block is not self.module:
                self.nested_def_statement(node)
                return
            has_defaults = bool(node.args.defaults) or any(
                default is not None for default in node.args.kw_defaults)
            if has_defaults:
                self.emit_default_values(node)
            if node.decorator_list:
                self.apply_decorators(node)
            return

        if isinstance(node, ast.Match):
            self.match_statement(node)
            return

        if isinstance(node, ast.ClassDef):
            return self.compile_class(node)

        if isinstance(node, ast.AsyncFor):
            raise CompileError("'async for' is not supported",
                               self.filename, node)

        if isinstance(node, ast.AsyncWith):
            raise CompileError("'async with' is not supported",
                               self.filename, node)

        if isinstance(node, ast.Pass):
            return

        if isinstance(node, ast.Assign):
            # 链式赋值 a = b = 1：值只求一次，再逐个存。
            self.expr(node.value)
            for index, target in enumerate(node.targets):
                if index + 1 < len(node.targets):
                    self.block.emit('DUP', node)
                self.assign_target(target, node)
            return

        if isinstance(node, ast.AnnAssign):
            if node.value is None:
                return
            self.expr(node.value)
            self.assign_target(node.target, node)
            return

        if isinstance(node, ast.AugAssign):
            self.augmented(node)
            return

        if isinstance(node, ast.Return):
            if node.value is None:
                self.block.emit('NONE', node)
            else:
                self.expr(node.value)
            # 返回值已在栈顶，但 __exit__ 会压/弹一层——先把值挪到临时槽，
            # 退出所有 with，再取回来。否则 WITH_EXIT 弹掉的正是返回值。
            if self.with_stack:
                temp = self.block.alloc_slot()
                self.block.emit('STORE_LOCAL', node)
                self.block.emit_u16(temp)
                self.emit_with_exits(node)
                self.block.emit('LOAD_LOCAL', node)
                self.block.emit_u16(temp)
            self.block.emit('RETURN', node)
            return

        if isinstance(node, ast.If):
            self.if_chain(node)
            return

        if isinstance(node, ast.While):
            # `while ... else`：条件变假后执行 else，break 跳过 else。
            loop_start = len(self.block.code)
            self.expr(node.test)
            self.block.emit('JUMP_IF_FALSE', node)
            exit_fixup = len(self.block.code)
            self.block.emit_u16(0)
            self.loop_stack.append((loop_start, [], False))
            for statement in node.body:
                self.statement(statement)
            _, breaks, _ = self.loop_stack.pop()
            self.block.emit('JUMP', node)
            self.block.emit_u16(loop_start)

            # 条件失败落在这里：先跑 else，再跳到末尾。
            else_start = len(self.block.code)
            self.block.patch_u16(exit_fixup, else_start)
            skip_fixup = None
            if node.orelse:
                for statement in node.orelse:
                    self.statement(statement)
                self.block.emit('JUMP', node)
                skip_fixup = len(self.block.code)
                self.block.emit_u16(0)
            exit_at = len(self.block.code)
            if skip_fixup is not None:
                self.block.patch_u16(skip_fixup, exit_at)
            for fixup in breaks:
                self.block.patch_u16(fixup, exit_at)
            return

        if isinstance(node, ast.For):
            self.for_loop(node)
            return

        if isinstance(node, ast.Break):
            if not self.loop_stack:
                raise CompileError('break 不在循环里', self.filename, node)
            continue_at, breaks, is_for = self.loop_stack[-1]
            # for 循环的栈上有 [列表, 下标] 两槽，break 时必须弹掉，
            # 否则下一次进循环会多两层残留。while 没有这个状态。
            #
            # 顺序：先退出 with（由内向外），再弹循环状态。反过来会让
            # __exit__ 在 for 的状态已被弹掉之后运行，出错时更难看。
            self.emit_with_exits(node)
            if is_for:
                self.block.emit('ITER_BREAK', node)
            self.block.emit('JUMP', node)
            fixup = len(self.block.code)
            self.block.emit_u16(0)
            breaks.append(fixup)
            return

        if isinstance(node, ast.Continue):
            if not self.loop_stack:
                raise CompileError('continue 不在循环里', self.filename, node)
            continue_at, _, _ = self.loop_stack[-1]
            # continue 跳回 ITER_NEXT（for）或循环开头（while）。
            # for 的 [列表, 下标] 仍在栈上，正好给 ITER_NEXT 用。
            self.emit_with_exits(node)
            self.block.emit('JUMP', node)
            self.block.emit_u16(continue_at)
            return

        if isinstance(node, ast.Global):
            # 槽在模块级已经分配好，这里不需要生成代码。
            return

        if isinstance(node, ast.Nonlocal):
            # 声明本身不产字节码：名字早已在 register_nested_functions 里
            # 被登记成捕获的自由变量。
            return

        if isinstance(node, ast.Assert):
            self.assert_statement(node)
            return

        if isinstance(node, ast.Raise):
            self.raise_statement(node)
            return

        if isinstance(node, ast.With):
            self.with_statement(node)
            return

        if isinstance(node, ast.Try):
            self.try_statement(node)
            return

        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            raise CompileError('不支持嵌套函数 "%s"；请移到模块顶层' % node.name,
                               self.filename, node)

        if isinstance(node, ast.Delete):
            self.delete_statement(node)
            return

        if isinstance(node, (ast.Import, ast.ImportFrom)):
            self.import_statement(node)
            return

        raise CompileError('不支持语句 %s' % type(node).__name__, self.filename, node)

    def delete_statement(self, node):
        """`del x` / `del x, y`。发射顺序与 C++ 版 deleteStatement 一致：

            DEL_LOCAL/DEL_GLOBAL <槽号>, <名字常量号>
        """
        for target in node.targets:
            if not isinstance(target, ast.Name):
                raise CompileError('del 只支持删除变量名', self.filename, target)
            name = target.id
            if self.is_local(name):
                self.block.emit('DEL_LOCAL', target)
                self.block.emit_u16(self.slot_of(name))
            else:
                self.block.emit('DEL_GLOBAL', target)
                self.block.emit_u16(self.global_slot(name))
            self.block.emit_u16(self.const_string(name))

    def assert_statement(self, node):
        """`assert cond` / `assert cond, msg`。

        发射顺序与 C++ 版 assertStatement 一致：

            <cond>
            JUMP_IF_FALSE L_THROW
            JUMP L_END
          L_THROW:
            <msg>              （有消息才发射）
            RAISE_TYPE "AssertionError", 有消息?
          L_END:
        """
        self.expr(node.test)
        self.block.emit('JUMP_IF_FALSE', node)
        throw_fixup = len(self.block.code)
        self.block.emit_u16(0)

        self.block.emit('JUMP', node)
        end_fixup = len(self.block.code)
        self.block.emit_u16(0)

        self.block.patch_u16(throw_fixup, len(self.block.code))
        has_message = node.msg is not None
        if has_message:
            self.expr(node.msg)
        self.block.emit('RAISE_TYPE', node)
        self.block.emit_u16(self.const_string('AssertionError'))
        self.block.emit_u8(1 if has_message else 0)

        self.block.patch_u16(end_fixup, len(self.block.code))

    def raise_statement(self, node):
        """`raise` / `raise X` / `raise X(msg)`。

        发射顺序必须与 C++ 版的 raiseStatement 一致：

            bare raise           → RERAISE
            raise ValueError     → RAISE_TYPE <常量号>, 0
            raise ValueError(m)  → <求值 m> ; RAISE_TYPE <常量号>, 1
            其它表达式           → <求值> ; RAISE
        """
        if node.exc is None:
            self.block.emit('RERAISE', node)
            return

        value = node.exc
        if (isinstance(value, ast.Call) and isinstance(value.func, ast.Name)
                and value.func.id in EXCEPTION_SET):
            if len(value.args) > 1:
                raise CompileError(
                    'raise %s(...) 这里最多一个参数；异常参数元组还不支持'
                    % value.func.id, self.filename, value)
            if value.args:
                self.expr(value.args[0])
            self.block.emit('RAISE_TYPE', node)
            self.block.emit_u16(self.const_string(value.func.id))
            self.block.emit_u8(1 if value.args else 0)
            return

        if isinstance(value, ast.Name) and value.id in EXCEPTION_SET:
            self.block.emit('RAISE_TYPE', node)
            self.block.emit_u16(self.const_string(value.id))
            self.block.emit_u8(0)
            return

        self.expr(value)
        self.block.emit('RAISE', node)

    def try_statement(self, node):
        """`try / except / else / finally`。

        ── 布局（与 C++ 版 tryStatement 一一对应）──

        有 finally 时，try/except/else 整段放在「finally 保护区」里，
        这样无论正常结束还是异常外传，控制流都先经过 finally 再由
        RERAISE 继续抛——CPython 就是这个语义。

            SETUP_EXC L_FIN           ← 外层：finally 的保护区
            [try/except/else 整段]
            POP_EXC
            <finally 体>
            JUMP L_END
          L_FIN:                      ← 异常进来时栈顶是异常值
            <finally 体>
            RAISE
          L_END:

        ── EXC_MATCH 的语义 ──

        「匹配则跳转」，所以 `except (A, B):` 排成两条，最后一条不匹配就
        自然落到下一个 handler。反过来写每条后面都要补一条 JUMP。
        """
        has_finally = bool(node.finalbody)

        finally_fixup = None
        if has_finally:
            self.block.emit('SETUP_EXC', node)
            finally_fixup = len(self.block.code)
            self.block.emit_u16(0)

        handler_fixup = None
        has_handlers = bool(node.handlers)
        if has_handlers:
            self.block.emit('SETUP_EXC', node)
            handler_fixup = len(self.block.code)
            self.block.emit_u16(0)

        for statement in node.body:
            self.statement(statement)

        if has_handlers:
            self.block.emit('POP_EXC', node)

        else_exit_fixup = None
        if has_handlers:
            self.block.emit('JUMP', node)
            else_exit_fixup = len(self.block.code)
            self.block.emit_u16(0)

        if has_handlers:
            self.block.patch_u16(handler_fixup, len(self.block.code))

            handler_exits = []
            for handler in node.handlers:
                if handler.type is not None:
                    types = self._exception_type_names(handler.type)
                    sites = len(self.block.code)
                    for name in types:
                        self.block.emit('EXC_MATCH', handler)
                        self.block.emit_u16(0)   # 先占位，稍后回填子句体起点
                        self.block.emit_u16(self.const_string(name))
                    body_at = len(self.block.code)
                    for k in range(len(types)):
                        # 每条 5 字节：操作码 + u16 目标 + u16 常量号
                        self.block.patch_u16(sites + k * 5 + 1, body_at)

                if handler.name is not None:
                    self.block.emit('CURRENT_EXC', handler)
                    self.name_store(handler.name, handler)

                for statement in handler.body:
                    self.statement(statement)
                self.block.emit('CLEAR_EXC', handler)

                self.block.emit('JUMP', handler)
                handler_exits.append(len(self.block.code))
                self.block.emit_u16(0)

            self.block.emit('RERAISE', node)
            self.block.patch_u16(else_exit_fixup, len(self.block.code))

            for statement in node.orelse:
                self.statement(statement)

            after_else = len(self.block.code)
            for fixup in handler_exits:
                self.block.patch_u16(fixup, after_else)

        if has_finally:
            self.block.emit('POP_EXC', node)
            for statement in node.finalbody:
                self.statement(statement)
            self.block.emit('JUMP', node)
            normal_fixup = len(self.block.code)
            self.block.emit_u16(0)

            self.block.patch_u16(finally_fixup, len(self.block.code))
            for statement in node.finalbody:
                self.statement(statement)
            self.block.emit('RAISE', node)

            self.block.patch_u16(normal_fixup, len(self.block.code))

    def _exception_type_names(self, type_node):
        """`except` 的类型表达式 → 名字列表。元组形式展开成多个。"""
        if isinstance(type_node, ast.Tuple):
            names = []
            for element in type_node.elts:
                names.extend(self._exception_type_names(element))
            return names
        if isinstance(type_node, ast.Name) and type_node.id in EXCEPTION_SET:
            return [type_node.id]
        name = getattr(type_node, 'id', '<expr>')
        raise CompileError('%s 不是异常类' % name, self.filename, type_node)

    def with_statement(self, node):
        """`with 管理器 as 名字:` —— 调 __enter__/__exit__。

        ── 为什么临时槽不回收 ──

        槽号分配是「用完就还」还是「只增不减」直接影响正确性：如果退出时
        把槽还回去，紧随其后的另一个 with 会复用同一槽，而外层的
        `__exit__` 还指着它——管理器被换掉，锁永远不释放。槽总数在几十
        量级，泄漏几个不影响性能，所以只增不减。

        ── 退出路径怎么覆盖 ──

        `with_stack` 记 [(槽号, ...)]。`return` / `break` / `continue` 生成
        时按**由内向外**给每层插一条 WITH_EXIT，因此在 with 体内任何位置
        跳出都会释放。这条不能省：锁不释放的症状是「程序偶发卡死」，而
        卡死点离 with 语句很远。
        """
        entered = []
        exit_fixups = []
        for item in node.items:
            if item.optional_vars is not None and not isinstance(item.optional_vars, ast.Name):
                raise CompileError('with ... as 后面只能跟一个名字',
                                   self.filename, item.optional_vars)

            # 管理器表达式先求值，WITH_ENTER 直接弹它。
            self.expr(item.context_expr)
            slot = self.block.alloc_slot()
            self.block.emit('WITH_ENTER', node)
            self.block.emit_u16(slot)
            # 第二个操作数：__exit__ 返回真值（吞掉异常）时跳到这里。
            exit_fixups.append(len(self.block.code))
            self.block.emit_u16(0)

            # `as 名字`：WITH_ENTER 把 __enter__ 的返回值留在栈上，存进名字即可。
            if item.optional_vars is not None:
                self.name_store(item.optional_vars.id, node)
            else:
                self.block.emit('POP', node)

            entered.append(slot)
            self.with_stack.append(slot)

        for statement in node.body:
            self.statement(statement)

        for slot in reversed(entered):
            self.with_stack.pop()
            self.block.emit('WITH_EXIT', node)
            self.block.emit_u16(slot)

        # 被吞掉的异常从这里继续。多条目时所有层指向同一终点。
        after = len(self.block.code)
        for fixup in exit_fixups:
            self.block.patch_u16(fixup, after)

    def emit_with_exits(self, node):
        """给 return / break / continue 插上「离开所有 with」的指令。

        由内向外：最内层的 with 先释放。
        """
        for slot in reversed(self.with_stack):
            self.block.emit('WITH_EXIT', node)
            self.block.emit_u16(slot)

    def if_chain(self, node):
        """if/elif/else。elif 在 AST 里是「orelse 只有一条 If」。"""
        exit_fixups = []
        current = node
        while True:
            self.expr(current.test)
            self.block.emit('JUMP_IF_FALSE', current)
            else_fixup = len(self.block.code)
            self.block.emit_u16(0)
            for statement in current.body:
                self.statement(statement)
            # 分支结束跳到整体末尾（除非它就是最后一段且没有 else）
            if current.orelse:
                self.block.emit('JUMP', current)
                exit_fixups.append(len(self.block.code))
                self.block.emit_u16(0)
            next_at = len(self.block.code)
            self.block.patch_u16(else_fixup, next_at)
            if len(current.orelse) == 1 and isinstance(current.orelse[0], ast.If):
                current = current.orelse[0]
                continue
            for statement in current.orelse:
                self.statement(statement)
            break
        exit_at = len(self.block.code)
        for fixup in exit_fixups:
            self.block.patch_u16(fixup, exit_at)

    def match_statement(self, node):
        """`match` 降级成 if/elif 链，与 C++ 版同一套生成。

        支持：字面量模式、`_` 通配、裸名字捕获、`as name`、或模式
        `X | Y`、`case P if G:` 守卫。类 / 序列 / 映射模式明确报错。
        """
        temp = self.block.slot_of('\0match_subject')
        self.expr(node.subject)
        self.block.emit('STORE_LOCAL', node)
        self.block.emit_u16(temp)

        end_fixups = []
        next_fixups = []
        case_count = len(node.cases)

        for index, clause in enumerate(node.cases):
            case_start = len(self.block.code)
            for fixup in next_fixups:
                self.block.patch_u16(fixup, case_start)
            next_fixups = []

            wildcard, bind_name = self.match_condition(
                clause.pattern, temp, clause)

            # 捕获模式没有匹配判据（总是命中），先绑定再判守卫——
            # `case n if n > 50:` 的守卫要能看到已绑定的 n。
            if wildcard and bind_name:
                self.block.emit('LOAD_LOCAL', clause.pattern)
                self.block.emit_u16(temp)
                self.name_store(bind_name, clause.pattern)
                bind_name = ''

            if not wildcard:
                # 行号取模式节点：`ast.match_case` 没有 lineno，用它会让行号
                # 表退化成 L0，与 C++ 版对不上。
                self.block.emit('JUMP_IF_FALSE', clause.pattern)
                next_fixups.append(len(self.block.code))
                self.block.emit_u16(0)

                # `case P as name:`：模式命中后先绑定，再判守卫。
                if bind_name:
                    self.block.emit('LOAD_LOCAL', clause.pattern)
                    self.block.emit_u16(temp)
                    self.name_store(bind_name, clause.pattern)
                    bind_name = ''

            # 守卫对通配/捕获模式同样要判：`case n if n > 50:` 必须真的看 n。
            if clause.guard is not None:
                self.expr(clause.guard)
                self.block.emit('JUMP_IF_FALSE', clause.guard)
                next_fixups.append(len(self.block.code))
                self.block.emit_u16(0)

            for statement in clause.body:
                self.statement(statement)

            if not wildcard or index + 1 < case_count:
                self.block.emit('JUMP', clause.pattern)
                end_fixups.append(len(self.block.code))
                self.block.emit_u16(0)

        end_at = len(self.block.code)
        for fixup in next_fixups:
            self.block.patch_u16(fixup, end_at)
        for fixup in end_fixups:
            self.block.patch_u16(fixup, end_at)

    def match_condition(self, pattern, temp, clause):
        """生成模式判据，返回 (是否通配, 捕获名)。

        `case _:` / `case name:` / `case X as name:` 都是「捕获形态」：
        前者总命中，后两者命中后把主题绑给 name。
        """
        bind_name = ''
        if isinstance(pattern, ast.MatchAs):
            # `case _:` → MatchAs(pattern=None, name=None)
            # `case n:` → MatchAs(pattern=None, name='n')
            # `case P as n:` → MatchAs(pattern=P, name='n')
            if pattern.pattern is None:
                return True, pattern.name or ''
            bind_name = pattern.name or ''
            return self.match_condition(pattern.pattern, temp, clause)[0], \
                bind_name

        if isinstance(pattern, ast.MatchOr):
            for sub in pattern.patterns:
                if not isinstance(sub, ast.MatchValue):
                    raise CompileError('match 里不支持类模式、序列模式和映射'
                                       '模式；请改用 if/elif', self.filename, sub)
            self.block.emit('LOAD_LOCAL', pattern)
            self.block.emit_u16(temp)
            for sub in pattern.patterns:
                self.expr(sub.value)
            self.block.emit('BUILD_TUPLE', pattern)
            self.block.emit_u16(len(pattern.patterns))
            self.block.emit('CMP', pattern)
            self.block.emit_u8(6)   # IN
            return False, ''

        if isinstance(pattern, ast.MatchValue):
            self.block.emit('LOAD_LOCAL', pattern)
            self.block.emit_u16(temp)
            self.expr(pattern.value)
            self.block.emit('CMP', pattern)
            self.block.emit_u8(0)   # EQ
            return False, ''

        raise CompileError('match 里不支持类模式、序列模式和映射模式；'
                           '请改用 if/elif', self.filename, pattern)

    def assign_target(self, target, node):
        if isinstance(target, ast.Attribute):
            # `a.b = v`：SET_ATTR 要 [接收者, 值]。先接住值再压接收者。
            temp = self.block.slot_of('\0assign_value')
            self.block.emit('STORE_LOCAL', node)
            self.block.emit_u16(temp)
            self.expr(target.value)
            self.block.emit('LOAD_LOCAL', node)
            self.block.emit_u16(temp)
            self.block.emit('SET_ATTR', node)
            self.block.emit_u16(self.const_string(target.attr))
            return
        if isinstance(target, ast.Name):
            # `f = ...` 覆盖一个脚本函数：上面那次「函数不能当值使用」的检查
            # 只在**读**名字时触发，写的时候不会。允许它会把函数静默替换成
            # 一个普通变量，之后所有 `f()` 调用都变成「调用一个非函数的值」。
            if target.id in self.function_by_name:
                raise CompileError(
                    '不允许给函数 %s 重新赋值；脚本里没有一等函数' % target.id,
                    self.filename, target)
            self.name_store(target.id, node)
            return
        if isinstance(target, ast.Subscript):
            # 栈上已有值；下标赋值要 [容器, 下标, 值] 的顺序。
            # 现在栈是 [值]，先用一个临时局部槽把它接住。
            temp = self.block.slot_of('\0assign_value')
            self.block.emit('STORE_LOCAL', node)
            self.block.emit_u16(temp)
            self.expr(target.value)
            if isinstance(target.slice, ast.Slice):
                # 切片赋值：压 [容器, start?, stop?, step?, 值]，再 SET_SLICE。
                self.slice_expr(target.slice, node, emit_slice=False)
                self.block.emit('LOAD_LOCAL', node)
                self.block.emit_u16(temp)
                self.block.emit('SET_SLICE', node)
                self.block.emit_u8(self.slice_flags(target.slice))
                return
            self.expr(target.slice)
            self.block.emit('LOAD_LOCAL', node)
            self.block.emit_u16(temp)
            self.block.emit('SET_ITEM', node)
            return
        if isinstance(target, (ast.Tuple, ast.List)):
            # 元组解包：栈上已有序列。UNPACK 拆成 N 个元素（第一个在最上），
            # 随后按**从后往前**的顺序逐个 assign_target。
            self.block.emit('UNPACK', target)
            self.block.emit_u16(len(target.elts))
            for element in reversed(target.elts):
                self.assign_target(element, node)
            return
        raise CompileError('不支持这种赋值目标', self.filename, target)

    def augmented(self, node):
        if isinstance(node.target, ast.Name):
            name = node.target.id
            self.name_target(name, node)
            self.expr(node.value)
            self.block.emit('BIN', node)
            self.block.emit_u8(self.binop_of(node.op, node))
            self.name_store(name, node)
            return
        if isinstance(node.target, ast.Subscript):
            self.expr(node.target.value)
            self.expr(node.target.slice)
            self.expr(node.value)
            self.block.emit('INPLACE', node)
            self.block.emit_u8(self.binop_of(node.op, node))
            return
        raise CompileError('不支持这种 += 目标', self.filename, node.target)

    def for_loop(self, node):
        tuple_target = isinstance(node.target, (ast.Tuple, ast.List))
        if not isinstance(node.target, ast.Name) and not tuple_target:
            raise CompileError('for 的循环变量必须是单个名字', self.filename,
                               node.target)

        self.expr(node.iter)
        self.block.emit('ITER_INIT', node)
        loop_top = len(self.block.code)
        self.block.emit('ITER_NEXT', node)
        exit_fixup = len(self.block.code)
        self.block.emit_u16(0)
        if tuple_target:
            # ITER_NEXT 压上来的元素本身是序列，交给 assign_target 拆。
            self.assign_target(node.target, node)
        else:
            self.name_store(node.target.id, node)

        self.loop_stack.append((loop_top, [], True))
        for statement in node.body:
            self.statement(statement)
        _, breaks, _ = self.loop_stack.pop()

        self.block.emit('JUMP', node)
        self.block.emit_u16(loop_top)

        # 迭代耗尽落在这里：先跑 else，再跳到末尾；break 直接到末尾。
        else_start = len(self.block.code)
        self.block.patch_u16(exit_fixup, else_start)
        skip_fixup = None
        if node.orelse:
            for statement in node.orelse:
                self.statement(statement)
            self.block.emit('JUMP', node)
            skip_fixup = len(self.block.code)
            self.block.emit_u16(0)
        exit_at = len(self.block.code)
        if skip_fixup is not None:
            self.block.patch_u16(skip_fixup, exit_at)
        for fixup in breaks:
            # break 时 ITER_BREAK 已经弹掉了循环状态，直接跳到出口。
            # 但 break 生成的是 ITER_BREAK + JUMP，JUMP 跳过的是 exit，
            # 而 exit 处不需要再弹栈，所以直接指向 exit_at 即可。
            self.block.patch_u16(fixup, exit_at)

    def format_spec_text(self, spec):
        """把 ast 的格式说明符（JoinedStr）拼回纯文本。

        CPython 把 `{x:.2f}` 的说明符也解析成 JoinedStr；这里只支持字面量
        形式（没有嵌套的 `{...}`），够覆盖 `:.2f` `:>5` 这类写法。
        """
        parts = []
        for item in getattr(spec, 'values', []):
            if isinstance(item, ast.Constant):
                parts.append(item.value)
            else:
                raise CompileError('不支持嵌套的格式说明符', self.filename, spec)
        return ''.join(parts)

    def import_statement(self, node):
        # 依赖图是构建期的事（见 pyui_extc）；字节码只知道「这个模块被用了」。
        #
        # 幂等：compile_module 会预扫一遍（让函数体编译时认得出扩展调用），
        # 模块级执行时又会走到这里。重复追加会让产物里的 import 表出现重复项，
        # 而且顺序不再等于「首次出现」。
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name not in self.imported:
                    self.imported.append(alias.name)
        else:
            module = node.module or ''
            if module and module not in self.imported:
                self.imported.append(module)

    # ── 表达式 ──

    def expr(self, node):
        if isinstance(node, ast.Constant):
            self.constant(node)
            return

        if isinstance(node, ast.Name):
            # `Ellipsis` 是内建常量，等价于 `...`。CPython 里它是单例，读多
            # 次得到同一个对象；这里编成无载荷常量，与 `...` 同一类型即可。
            # 判据与 C++ 的 shadowed 一致：模块级看编译顺序，函数体看模块级
            # 是否声明过——`Ellipsis = 5` 之后它就是个普通变量。
            if node.id == 'Ellipsis':
                if self.block is self.module:
                    hidden = (self.is_local(node.id) or
                              node.id in self.assigned_so_far)
                else:
                    hidden = (self.is_local(node.id) or
                              node.id in self.globals)
                if not hidden:
                    self.block.emit('CONST', node)
                    self.block.emit_u16(self.const_index(Ellipsis))
                    return
            # 同名变量优先于类型名：`int = 5` 之后再读 `int` 必须是 5，
            # 不是 `<class 'int'>`——CPython 里赋值遮蔽内建。
            #
            # 判据分两种：模块级代码按**编译顺序**看已赋值的名字（`int = 5`
            # 写在后面时前面的 `int` 仍是内建）；函数体里按模块级是否
            # **声明过**——函数调用发生在模块执行完之后。
            if self.block is self.module:
                shadowed = (self.is_local(node.id) or
                            node.id in self.assigned_so_far)
            else:
                shadowed = (self.is_local(node.id) or
                            node.id in self.globals)
            # 类型名在值位置是合法的：`type(x) is int`、`isinstance(x, str)`。
            # 与 C++ 版同序——必须在「内建不能当值」之前判。
            if not shadowed and (node.id in BUILTIN_TYPE_NAMES or
                                 node.id in EXCEPTION_SET):
                self.block.emit('LOAD_TYPE', node)
                self.block.emit_u16(self.const_string(node.id))
                return
            if node.id in BUILTIN_INDEX:
                # 内建名被赋值过时它就是一个普通变量，不再是「不能当值」的内建。
                if shadowed:
                    self.name_target(node.id, node)
                    return
                # 内建函数是一等值（`sorted(x, key=len)`、`f = len`）。
                self.block.emit('MAKE_BUILTIN_FUNCTION', node)
                self.block.emit_u16(self.builtin_index(node.id))
                return
            # 脚本函数出现在值的位置（`g = f`、`f(func)` 的实参）就是取函数值。
            # 带装饰器的名字存的是装饰器返回值，读它就是读变量。
            if (node.id in self.function_by_name and
                    node.id not in self.decorated_names):
                self.block.emit('MAKE_FUNCTION', node)
                self.block.emit_u16(self.function_by_name[node.id])
                self.block.emit_u16(0)
                return
            self.name_target(node.id, node)
            return

        if isinstance(node, ast.BinOp):
            self.expr(node.left)
            self.expr(node.right)
            self.block.emit('BIN', node)
            self.block.emit_u8(self.binop_of(node.op, node))
            return

        if isinstance(node, ast.UnaryOp):
            self.expr(node.operand)
            self.block.emit('UNARY', node)
            self.block.emit_u8(self.unop_of(node.op, node))
            return

        if isinstance(node, ast.BoolOp):
            # and/or 返回操作数本身，且短路：值留在栈上，条件决定跳不跳。
            #
            # 每条 JUMP_IF_*_KEEP 后面必须紧跟 POP：短路命中时值要**留在
            # 栈上**当结果（所以用 KEEP 版跳转），而没命中时要把它弹掉再去
            # 求下一个操作数。少了这条 POP，`a and b` 的两个操作数会同时
            # 留在栈上——`print("x: " + str(a and b))` 会输出 "TrueTrue"，
            # 而单独写 `x = a and b` 只是把 a 静默丢在栈里，症状更隐蔽。
            is_and = isinstance(node.op, ast.And)
            keep = 'JUMP_IF_FALSE_KEEP' if is_and else 'JUMP_IF_TRUE_KEEP'
            fixups = []
            for value in node.values[:-1]:
                self.expr(value)
                self.block.emit(keep, node)
                fixups.append(len(self.block.code))
                self.block.emit_u16(0)
                self.block.emit('POP', node)
            self.expr(node.values[-1])
            end = len(self.block.code)
            for fixup in fixups:
                self.block.patch_u16(fixup, end)
            return

        if isinstance(node, ast.Compare):
            # 链式比较 `a < b < c`：中间项只求值一次。
            #
            # 用两个临时局部槽存「上一次的右值」和「当前右值」。为什么不用栈
            # 上的 DUP 复制：VM 没有 SWAP 指令，要同时保住「比较结果」和
            # 「右值当下一轮的左值」就得靠两三条 DUP/POP 凑，一旦算错偏移
            # 会静默算错结果。临时槽慢一点点，但读代码的人一眼能看懂。
            if len(node.comparators) == 1:
                self.expr(node.left)
                self.expr(node.comparators[0])
                self.block.emit('CMP', node)
                self.block.emit_u8(self.cmpop_of(node.ops[0], node))
                return

            left_slot = self.block.slot_of('\0chained_left')
            right_slot = self.block.slot_of('\0chained_right')
            self.expr(node.left)
            self.block.emit('STORE_LOCAL', node)
            self.block.emit_u16(left_slot)

            false_fixups = []
            for index, (op, comparator) in enumerate(zip(node.ops, node.comparators)):
                self.expr(comparator)
                self.block.emit('STORE_LOCAL', node)
                self.block.emit_u16(right_slot)
                self.block.emit('LOAD_LOCAL', node)
                self.block.emit_u16(left_slot)
                self.block.emit('LOAD_LOCAL', node)
                self.block.emit_u16(right_slot)
                self.block.emit('CMP', node)
                self.block.emit_u8(self.cmpop_of(op, node))

                if index + 1 == len(node.comparators):
                    # 最后一个比较的结果就是整个表达式的结果。
                    break

                # 中间结果只用来决定是否继续；不继续时整个表达式是 False。
                self.block.emit('JUMP_IF_FALSE', node)
                false_fixups.append(len(self.block.code))
                self.block.emit_u16(0)
                # 继续：右值成为下一轮的左值。
                self.block.emit('LOAD_LOCAL', node)
                self.block.emit_u16(right_slot)
                self.block.emit('STORE_LOCAL', node)
                self.block.emit_u16(left_slot)

            self.block.emit('JUMP', node)
            end_fixup = len(self.block.code)
            self.block.emit_u16(0)

            false_at = len(self.block.code)
            for fixup in false_fixups:
                self.block.patch_u16(fixup, false_at)
            self.block.emit('FALSE', node)

            end_at = len(self.block.code)
            self.block.patch_u16(end_fixup, end_at)
            return

        if isinstance(node, ast.IfExp):
            # x if c else y：只求值一支。
            self.expr(node.test)
            self.block.emit('JUMP_IF_FALSE', node)
            else_fixup = len(self.block.code)
            self.block.emit_u16(0)
            self.expr(node.body)
            self.block.emit('JUMP', node)
            end_fixup = len(self.block.code)
            self.block.emit_u16(0)
            else_at = len(self.block.code)
            self.block.patch_u16(else_fixup, else_at)
            self.expr(node.orelse)
            end_at = len(self.block.code)
            self.block.patch_u16(end_fixup, end_at)
            return

        if isinstance(node, ast.Call):
            self.call(node)
            return

        if isinstance(node, ast.Attribute):
            # Activity 的方法体里 `self.x` 已被 MethodFlattener 改写成名字；
            # 走到这里说明是真正的属性读（脚本类实例或类对象）。
            self.expr(node.value)
            self.block.emit('GET_ATTR', node)
            self.block.emit_u16(self.const_string(node.attr))
            return

        if isinstance(node, ast.Subscript):
            self.expr(node.value)
            if isinstance(node.slice, ast.Slice):
                self.slice_expr(node.slice, node)
            else:
                self.expr(node.slice)
                self.block.emit('GET_ITEM', node)
            return

        if isinstance(node, ast.List):
            for element in node.elts:
                self.expr(element)
            self.block.emit('BUILD_LIST', node)
            self.block.emit_u16(len(node.elts))
            return

        if isinstance(node, ast.Set):
            for element in node.elts:
                self.expr(element)
            self.block.emit('BUILD_SET', node)
            self.block.emit_u16(len(node.elts))
            return

        if isinstance(node, ast.Dict):
            if any(key is None for key in node.keys):
                raise CompileError('不支持字典展开 **', self.filename, node)
            for key, value in zip(node.keys, node.values):
                self.expr(key)
                self.expr(value)
            self.block.emit('BUILD_DICT', node)
            self.block.emit_u16(len(node.keys))
            return

        if isinstance(node, ast.JoinedStr):
            # 与 C++ 版一致：常量段直接压常量，表达式段先 str() 再 ADD 链。
            emitted = False
            for part in node.values:
                if isinstance(part, ast.Constant):
                    self.block.emit('CONST', node)
                    self.block.emit_u16(self.const_string(part.value))
                else:
                    # ast.FormattedValue
                    # `!r` / `!s` / `!a` 包成 repr(x) / str(x)，与 C++ 版同形。
                    # 合成节点要带上原表达式的行号，否则行号表退化成 L0，
                    # 与 C++ 版对不上。
                    value_node = part.value
                    if part.conversion in (114, 97):        # 'r', 'a'
                        value_node = ast.Call(
                            func=ast.Name(id='repr', ctx=ast.Load()),
                            args=[part.value], keywords=[])
                        ast.copy_location(value_node, part.value)
                    elif part.conversion == 115:            # 's'
                        value_node = ast.Call(
                            func=ast.Name(id='str', ctx=ast.Load()),
                            args=[part.value], keywords=[])
                        ast.copy_location(value_node, part.value)
                    if part.format_spec is not None:
                        # `f"{x:.2f}"` 编成 `("{:.2f}").format(x)`：
                        # 与 str.format 共用同一套格式说明符实现。
                        spec = self.format_spec_text(part.format_spec)
                        self.block.emit('CONST', node)
                        self.block.emit_u16(self.const_string('{:' + spec + '}'))
                        self.expr(value_node)
                        self.block.emit('CALL_METHOD', node)
                        self.block.emit_u16(self.const_string('format'))
                        self.block.emit_u8(1)
                    else:
                        self.expr(value_node)
                        self.block.emit('CALL_BUILTIN', node)
                        self.block.emit_u16(self.builtin_index('str'))
                        self.block.emit_u8(1)
                if emitted:
                    self.block.emit('BIN', node)
                    self.block.emit_u8(BINOP['ADD'])
                emitted = True
            if not emitted:
                self.block.emit('CONST', node)
                self.block.emit_u16(self.const_string(''))
            return

        if isinstance(node, ast.Tuple):
            for element in node.elts:
                self.expr(element)
            self.block.emit('BUILD_TUPLE', node)
            self.block.emit_u16(len(node.elts))
            return

        if isinstance(node, ast.Lambda):
            index = self.register_lambda(node)
            block = self.functions[index]['block']
            if block.default_slot >= 0:
                self.emit_lambda_defaults(node, block)
            for name in block.captures:
                self.name_target(name, node)
            self.block.emit('MAKE_FUNCTION', node)
            self.block.emit_u16(index)
            self.block.emit_u16(len(block.captures))
            return

        if isinstance(node, ast.YieldFrom):
            raise CompileError('不支持 yield from', self.filename, node)

        if isinstance(node, ast.Await):
            # `await 协程`：求值协程对象，再让 VM 驱动它跑到底取返回值。
            self.expr(node.value)
            self.block.emit('AWAIT', node)
            return

        if isinstance(node, ast.Yield):
            # `yield` / `yield x`：求出值（没有操作数就是 None）后发 YIELD。
            if node.value is None:
                self.block.emit('NONE', node)
            else:
                self.expr(node.value)
            self.block.emit('YIELD', node)
            return

        if isinstance(node, ast.NamedExpr):
            # `(n := 5)`：存一份、留一份，所以 `print(n := 3)` 既打印 3
            # 也绑定了 n。C++ 版同形（DUP + STORE）。
            if not isinstance(node.target, ast.Name):
                raise CompileError('赋值表达式的目标必须是名字',
                                   self.filename, node)
            self.expr(node.value)
            self.block.emit('DUP', node)
            self.name_store(node.target.id, node)
            return

        raise CompileError('不支持表达式 %s' % type(node).__name__, self.filename, node)

    def slice_flags(self, node):
        """位标记 1=start 2=stop 4=step，与 SLICE / SET_SLICE 指令一致。"""
        flags = 0
        if node.lower is not None:
            flags |= 1
        if node.upper is not None:
            flags |= 2
        if node.step is not None:
            flags |= 4
        return flags

    def slice_expr(self, node, parent, emit_slice=True):
        """切片 x[a:b:c]。压栈顺序 容器,start,stop,step。

        emit_slice=False 时只压操作数，供切片赋值拼 SET_SLICE 用。
        """
        # 容器已经在栈上，接下来的操作数直接压上去。
        if node.lower is not None:
            self.expr(node.lower)
        if node.upper is not None:
            self.expr(node.upper)
        if node.step is not None:
            self.expr(node.step)
        if emit_slice:
            self.block.emit('SLICE', parent)
            self.block.emit_u8(self.slice_flags(node))

    def call_dynamic(self, node, index):
        """签名含可变参数或实参有 `*`/`**` 展开：走 CALL_DYN。"""
        self.block.emit('BUILD_LIST', node)
        self.block.emit_u16(0)
        for argument in node.args:
            if isinstance(argument, ast.Starred):
                self.expr(argument.value)
                self.block.emit('CALL_BUILTIN', argument)
                self.block.emit_u16(self.builtin_index('list'))
                self.block.emit_u8(1)
            else:
                self.expr(argument)
                self.block.emit('BUILD_LIST', argument)
                self.block.emit_u16(1)
            self.block.emit('BIN', argument)
            self.block.emit_u8(BINOPS.index('ADD'))

        self.block.emit('BUILD_DICT', node)
        self.block.emit_u16(0)
        for keyword in node.keywords:
            if keyword.arg is not None:
                self.block.emit('DUP', keyword.value)
                self.block.emit('CONST', keyword.value)
                self.block.emit_u16(self.const_string(keyword.arg))
                self.expr(keyword.value)
                self.block.emit('SET_ITEM', keyword.value)
            else:
                self.expr(keyword.value)
                self.block.emit('DICT_MERGE', keyword.value)

        self.block.emit('CALL_DYN', node)
        self.block.emit_u16(index)

    def call_value_dyn(self, node):
        """按值调用的动态版本：可调用值已在栈底，再压 [位置列表, 关键字字典]。

        与 call_dynamic 同构，只是末尾不发函数号——目标到运行期才知道。
        """
        self.block.emit('BUILD_LIST', node)
        self.block.emit_u16(0)
        for argument in node.args:
            if isinstance(argument, ast.Starred):
                self.expr(argument.value)
                self.block.emit('CALL_BUILTIN', argument)
                self.block.emit_u16(self.builtin_index('list'))
                self.block.emit_u8(1)
            else:
                self.expr(argument)
                self.block.emit('BUILD_LIST', argument)
                self.block.emit_u16(1)
            self.block.emit('BIN', argument)
            self.block.emit_u8(BINOPS.index('ADD'))

        self.block.emit('BUILD_DICT', node)
        self.block.emit_u16(0)
        for keyword in node.keywords:
            if keyword.arg is not None:
                self.block.emit('DUP', keyword.value)
                self.block.emit('CONST', keyword.value)
                self.block.emit_u16(self.const_string(keyword.arg))
                self.expr(keyword.value)
                self.block.emit('SET_ITEM', keyword.value)
            else:
                self.expr(keyword.value)
                self.block.emit('DICT_MERGE', keyword.value)

        self.block.emit('CALL_VALUE_DYN', node)

    def call_value_args(self, node):
        """按值调用的实参编码（可调用值已在栈底）。

        没有 `*`/`**` 就直接压实参；有展开就压 [位置列表, 关键字字典]。
        """
        dynamic = False
        for argument in node.args:
            if isinstance(argument, ast.Starred):
                dynamic = True
        if node.keywords:
            dynamic = True

        if not dynamic:
            for argument in node.args:
                self.expr(argument)
            self.block.emit('CALL_VALUE', node)
            self.block.emit_u8(len(node.args))
            return

        self.call_value_dyn(node)

    def call(self, node):
        # `super()`：当前方法所属的类 + self。
        if (isinstance(node.func, ast.Name) and node.func.id == 'super' and
                not node.args and not node.keywords):
            if not self.current_class:
                raise CompileError('super() 只能用在类方法里',
                                   self.filename, node)
            self.block.emit('LOAD_LOCAL', node)
            self.block.emit_u16(0)
            self.block.emit('MAKE_SUPER', node)
            self.block.emit_u16(self.const_string(self.current_class))
            return

        # `A(...)`：脚本类 → INSTANTIATE。
        if (isinstance(node.func, ast.Name) and
                node.func.id in self.class_names):
            if node.keywords:
                raise CompileError('不支持关键字传参；按位置传', self.filename, node)
            self.name_target(node.func.id, node.func)
            for argument in node.args:
                self.expr(argument)
            self.block.emit('INSTANTIATE', node)
            self.block.emit_u8(len(node.args))
            return

        # 内建：len/str/range… 编译成 CALL_BUILTIN + 下标。
        # 带关键字实参时走 CALL_BUILTIN_KW：内建签名没有形参名，由 VM 按
        # builtins_keywords.cpp 的表把名字换成位置。
        if isinstance(node.func, ast.Name) and node.func.id in BUILTIN_INDEX:
            if node.keywords:
                for argument in node.args:
                    self.expr(argument)
                for keyword in node.keywords:
                    if keyword.arg is None:
                        raise CompileError('不支持 ** 展开到内建调用',
                                           self.filename, node)
                    self.block.emit('CONST', keyword)
                    self.block.emit_u16(self.const_string(keyword.arg))
                    self.expr(keyword.value)
                self.block.emit('CALL_BUILTIN_KW', node)
                self.block.emit_u16(self.builtin_index(node.func.id))
                self.block.emit_u8(len(node.args))
                self.block.emit_u8(len(node.keywords))
                return
            for argument in node.args:
                self.expr(argument)
            self.block.emit('CALL_BUILTIN', node)
            self.block.emit_u16(self.builtin_index(node.func.id))
            self.block.emit_u8(len(node.args))
            return

        # 脚本内函数：CALL + 函数号。关键字参数在编译期重排成位置顺序：
        # 脚本函数的签名已知，VM 不需要认识关键字。
        # 带装饰器的名字不走这里：它运行期装的是装饰器返回值。
        if (isinstance(node.func, ast.Name) and
                node.func.id in self.function_by_name and
                node.func.id not in self.decorated_names):
            entry = self.functions[self.function_by_name[node.func.id]]
            block = entry['block']
            params = []
            for slot in range(block.param_count):
                for name, index in block.locals.items():
                    if index == slot:
                        params.append(name)
                        break

            # 形参含可变参数 / 仅关键字参数，或实参有 `*` / `**` 展开时，
            # 实参的个数与名字要到运行期才知道，改发 [位置列表, 关键字字典]。
            dynamic = (block.vararg_slot >= 0 or block.kwarg_slot >= 0 or
                       block.kwonly_start >= 0)
            # 有位置-only 参数时必须走动态绑定：静态路径按名字把关键字
            # 拍到位置上，会静默接受 `f(a=1)`（CPython 报错）。
            if block.posonly_count > 0:
                dynamic = True
            if not dynamic:
                for argument in node.args:
                    if isinstance(argument, ast.Starred):
                        dynamic = True
                for keyword in node.keywords:
                    if keyword.arg is None:
                        dynamic = True
            if dynamic:
                self.call_dynamic(node, self.function_by_name[node.func.id])
                return

            # 位置参数段只到 kwonly_start（没有仅关键字参数时就是全部）。
            positional_count = (block.kwonly_start if block.kwonly_start >= 0
                                else len(params))
            ordered = [None] * len(params)
            positional = 0
            highest = 0
            for argument in node.args:
                if positional >= positional_count:
                    raise CompileError('函数 "%s" 收到过多参数' % node.func.id,
                                       self.filename, argument)
                if ordered[positional] is not None:
                    raise CompileError('参数 "%s" 被重复传入' % params[positional],
                                       self.filename, argument)
                ordered[positional] = argument
                positional += 1
                highest = max(highest, positional)
            for keyword in node.keywords:
                if keyword.arg not in params:
                    raise CompileError('函数 "%s" 收到未预期的关键字参数 "%s"'
                                       % (node.func.id, keyword.arg),
                                       self.filename, keyword.value)
                slot = params.index(keyword.arg)
                if ordered[slot] is not None:
                    raise CompileError('参数 "%s" 被重复传入' % keyword.arg,
                                       self.filename, keyword.value)
                ordered[slot] = keyword.value
                highest = max(highest, slot + 1)
            for index in range(highest):
                if ordered[index] is not None:
                    continue
                if not (block.default_mask >> index) & 1:
                    raise CompileError('函数 "%s" 缺少必需参数 "%s"'
                                       % (node.func.id, params[index]),
                                       self.filename, node)
            for index in range(highest):
                if ordered[index] is not None:
                    self.expr(ordered[index])
                    continue
                # 洞有默认值：从隐藏槽按下标取这一个。
                self.block.emit('LOAD_GLOBAL', node)
                self.block.emit_u16(block.default_slot)
                self.block.emit('CONST', node)
                self.block.emit_u16(self.const_index(index))
                self.block.emit('GET_ITEM', node)
            self.block.emit('CALL', node)
            self.block.emit_u16(self.function_by_name[node.func.id])
            self.block.emit_u8(highest)
            return

        # 调用一个变量：`g(1)`，其中 g 是存着函数值的变量。
        # 与 C++ 版同序：放在裸名分支之前，否则会被当成扩展函数。
        if isinstance(node.func, ast.Name):
            is_variable = (self.is_local(node.func.id) or
                           node.func.id in self.globals)
            if is_variable:
                self.name_target(node.func.id, node.func)
                return self.call_value_args(node)

        # 异常类名调用：`ValueError("x")` 构造异常实例。
        # 与 C++ 版同序：放在裸名扩展分支之前，否则会被当成库函数。
        if isinstance(node.func, ast.Name) and node.func.id in EXCEPTION_SET:
            is_variable = (self.is_local(node.func.id) or
                           node.func.id in self.globals)
            if not is_variable:
                if node.keywords:
                    raise CompileError(
                        'unsupported keyword arguments; pass them positionally',
                        self.filename, node)
                self.block.emit('LOAD_TYPE', node.func)
                self.block.emit_u16(self.const_index(node.func.id))
                for argument in node.args:
                    self.expr(argument)
                self.block.emit('CALL_VALUE', node)
                self.block.emit_u8(len(node.args))
                return

        # 裸名：可能是库注册的内建（`open`）
        #
        # 走到这里说明这个名字既不是语言内建、也不是脚本内函数。它可能是一个
        # 库用 AIPY_PYLIB_BUILTIN 注册的短名（`open` → `fs.open`）。
        #
        # 这里不拒绝：编译器按设计不认识具体能力，而库是可插拔的。编成按名字
        # 的扩展调用，由打包期决定能不能解析；解析不到就报
        # 「没有任何库声明这个名字」。代价是拼写错误从编译期推迟到打包期，
        # 换来的是「新增裸名不用改编译器」。
        if isinstance(node.func, ast.Name):
            if node.keywords:
                raise CompileError('不支持关键字传参；按位置传', self.filename, node)
            for argument in node.args:
                self.expr(argument)
            self.block.emit('CALL_EXT', node)
            self.block.emit_u16(self.extension_index(node.func.id))
            self.block.emit_u8(len(node.args))
            return

        # 扩展：模块.函数(...)
        if isinstance(node.func, ast.Attribute):
            # 接收者是扩展模块名时走 CALL_EXT；否则是方法调用。
            #
            # 模块名可以是点分的（`os.path.exists`）：取接收者的完整属性链，
            # 再判断它是不是 import 过的模块（含点分前缀匹配——`import os`
            # 之后 `os.path` 就可用，与 CPython 一致）。
            receiver_name = self.dotted_name(node.func.value)
            if receiver_name and self.is_module(receiver_name):
                if node.keywords:
                    raise CompileError('不支持关键字传参；按位置传',
                                       self.filename, node)
                # 限定名进产物；函数指针由加载器按名字从已链接的 pylib 里查。
                # 产物里不出现任何地址——同一个脚本换一套 pylib 链接组合也照样跑。
                for argument in node.args:
                    self.expr(argument)
                qualified = receiver_name + '.' + node.func.attr
                self.block.emit('CALL_EXT', node)
                self.block.emit_u16(self.extension_index(qualified))
                self.block.emit_u8(len(node.args))
                return
            # 压栈顺序：接收者在下、参数在上。VM 从栈顶弹参数、
            # 最后弹接收者，所以接收者必须先压。
            self.expr(node.func.value)
            if node.keywords:
                # 带关键字的方法调用：`x.sort(key=f, reverse=True)`。
                # 哪些方法收关键字是运行期语义，由 VM 明确报错。
                for argument in node.args:
                    self.expr(argument)
                for keyword in node.keywords:
                    self.block.emit('CONST', keyword)
                    self.block.emit_u16(self.const_string(keyword.arg))
                    self.expr(keyword.value)
                self.block.emit('CALL_METHOD_KW', node)
                self.block.emit_u16(self.const_string(node.func.attr))
                self.block.emit_u8(len(node.args))
                self.block.emit_u8(len(node.keywords))
                return
            for argument in node.args:
                self.expr(argument)
            self.block.emit('CALL_METHOD', node)
            self.block.emit_u16(self.const_string(node.func.attr))
            self.block.emit_u8(len(node.args))
            return

        # 其它任何表达式当调用目标：`f()(1)`、`(lambda x: x)(1)`。
        # 先求值拿到函数值，再按值调用——与「调用变量」同一套指令。
        self.expr(node.func)
        return self.call_value_args(node)

    def constant(self, node):
        value = node.value
        if value is None:
            self.block.emit('NONE', node)
            return
        if value is True:
            self.block.emit('TRUE', node)
            return
        if value is False:
            self.block.emit('FALSE', node)
            return
        if isinstance(value, int):
            self.block.emit('CONST', node)
            self.block.emit_u16(self.const_index(value))
            return
        if isinstance(value, float):
            self.block.emit('CONST', node)
            self.block.emit_u16(self.const_index(value))
            return
        if isinstance(value, str):
            self.block.emit('CONST', node)
            self.block.emit_u16(self.const_string(value))
            return
        if isinstance(value, bytes):
            self.block.emit('CONST', node)
            self.block.emit_u16(self.const_index(value))
            return
        if value is Ellipsis:
            self.block.emit('CONST', node)
            self.block.emit_u16(self.const_index(value))
            return
        raise CompileError('不支持字面量 %r' % (value,), self.filename, node)

    # ── 算子映射 ──

    def binop_of(self, op, node):
        table = {
            ast.Add: 'ADD', ast.Sub: 'SUB', ast.Mult: 'MUL', ast.Div: 'DIV',
            ast.FloorDiv: 'FLOOR_DIV', ast.Mod: 'MOD', ast.Pow: 'POW',
            ast.BitAnd: 'BIT_AND', ast.BitOr: 'BIT_OR', ast.BitXor: 'BIT_XOR',
            ast.LShift: 'SHL', ast.RShift: 'SHR',
        }
        name = table.get(type(op))
        if name is None:
            raise CompileError('不支持运算符 %s' % type(op).__name__, self.filename, node)
        return BINOP[name]

    def cmpop_of(self, op, node):
        table = {
            ast.Eq: 'EQ', ast.NotEq: 'NE', ast.Lt: 'LT', ast.LtE: 'LE',
            ast.Gt: 'GT', ast.GtE: 'GE', ast.In: 'IN', ast.NotIn: 'NOT_IN',
            ast.Is: 'IS', ast.IsNot: 'IS_NOT',
        }
        name = table.get(type(op))
        if name is None:
            raise CompileError('不支持比较 %s' % type(op).__name__, self.filename, node)
        return CMPOP[name]

    def unop_of(self, op, node):
        table = {ast.USub: 'NEG', ast.Not: 'NOT', ast.Invert: 'INVERT'}
        name = table.get(type(op))
        if name is None:
            raise CompileError('不支持一元运算 %s' % type(op).__name__,
                               self.filename, node)
        return UNOP[name]

    # ── 产出 ──

    def build_program(self):
        def block_dict(block):
            return {
                'name': block.name,
                'code': list(block.code),
                'lines': [[offset, line] for offset, line in block.lines],
                'paramCount': block.param_count,
                'localCount': block.local_count,
                'defaultCount': block.default_count,
                'defaultSlot': block.default_slot,
                'defaultMask': block.default_mask,
                'varargSlot': block.vararg_slot,
                'kwargSlot': block.kwarg_slot,
                'kwonlyStart': block.kwonly_start,
                'posonlyCount': block.posonly_count,
                'captureStart': block.capture_start,
                'captureNames': block.capture_names,
                'isGenerator': block.is_generator,
                'isCoroutine': block.is_coroutine,
                'paramNames': block.param_names(),
            }

        return {
            'format': 'aipy-bytecode/1',
            'module': self.module_name,
            'code': list(self.module.code),
            'lines': [[offset, line] for offset, line in self.module.lines],
            'localCount': self.module.local_count,
            'globalCount': self.global_count,
            'globals': sorted(self.globals, key=self.globals.get),
            'consts': [self.json_const(value) for value in self.consts],
            'functions': [block_dict(entry['block']) for entry in self.functions],
            'imports': self.imported,
            # 扩展表：只放**实际调用到**的「模块.函数」限定名，顺序就是
            # CALL_EXT 的下标。加载器按名字去已链接的 pylib 里查指针，
            # 所以产物里不含地址，换链接组合不用重新编译脚本。
            'extensions': self.extensions,
            # 内建表：只有**实际用到**的才进产物，顺序就是 CALL_BUILTIN 的下标。
            # 这样字节码与运行时的映射是显式的：加载器按名字查函数指针，
            # 查不到就报错，不会悄悄调用到别的函数。
            'builtins': self.used_builtins,
        }

    def json_const(self, value):
        if value is Ellipsis:
            return {'type': 'ellipsis'}
        if isinstance(value, bool):
            return {'type': 'bool', 'value': value}
        if isinstance(value, int):
            return {'type': 'int', 'value': value}
        if isinstance(value, float):
            return {'type': 'float', 'value': value}
        if isinstance(value, bytes):
            return {'type': 'bytes', 'value': value}
        return {'type': 'str', 'value': value}


def compile_source(source, filename='<script>', module_name='main'):
    """把源码编译成可序列化的字节码描述（dict）。"""
    tree = ast.parse(source, filename=filename)
    compiler = Compiler(filename, module_name)
    return compiler.compile_module(tree)


def compile_file(path, module_name=None):
    with io.open(path, 'r', encoding='utf-8') as handle:
        source = handle.read()
    if module_name is None:
        module_name = path.replace('\\', '/').split('/')[-1].rsplit('.', 1)[0]
    return compile_source(source, filename=path, module_name=module_name)


def main(argv):
    if len(argv) < 2:
        print('用法: compiler.py <script.py> [out.json]', file=sys.stderr)
        return 2
    path = argv[1]
    try:
        program = compile_file(path)
    except CompileError as error:
        print(str(error), file=sys.stderr)
        return 1
    text = json.dumps(program, ensure_ascii=False, indent=1)
    if len(argv) > 2:
        with io.open(argv[2], 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(text)
        print('已写出 %s（%d 条指令，%d 个函数）'
              % (argv[2], len(program['code']), len(program['functions'])))
    else:
        print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
