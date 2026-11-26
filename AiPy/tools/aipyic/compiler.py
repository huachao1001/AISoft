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
    'format_text', 'char_from_code', 'text_of_char', 'set',
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
        self.globals = {}              # 模块级变量名 → 全局槽
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
        """写出一个名字：局部优先，否则当全局。"""
        if self.is_local(name):
            self.block.emit('LOAD_LOCAL', node)
            self.block.emit_u16(self.block.slot_of(name))
        else:
            self.block.emit('LOAD_GLOBAL', node)
            self.block.emit_u16(self.global_slot(name))

    def name_store(self, name, node):
        if self.is_local(name):
            self.block.emit('STORE_LOCAL', node)
            self.block.emit_u16(self.block.slot_of(name))
        else:
            self.block.emit('STORE_GLOBAL', node)
            self.block.emit_u16(self.global_slot(name))

    # ── 入口 ──

    def compile_module(self, tree):
        body = list(tree.body)
        # Activity 类：脚本的入口形式。类体赋值是字段（→ 全局槽），
        # 类里的方法编译成普通函数，`self` 参数被去掉——一个脚本只有一个
        # 实例，`self` 与「模块状态」是同一件事。
        activity = None
        for node in body:
            if isinstance(node, ast.ClassDef):
                if activity is not None:
                    raise CompileError('只能有一个 Activity 类', self.filename, node)
                activity = node

        if activity is not None:
            body = self.expand_activity(activity, body)

        # 先把所有顶层 def 收进来：Python 允许「先调用后定义」，而模块级
        # 代码按顺序执行，所以函数必须先全部就位。
        for node in body:
            if isinstance(node, ast.FunctionDef):
                self.register_function(node)

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
            if isinstance(node, ast.FunctionDef):
                self.compile_function_body(node)

        # 模块级语句：按源码顺序执行。放在函数编译之后，这样
        # `g = f`（读函数名）与 `f = 3`（覆盖函数）都能被拒绝。
        for node in body:
            if isinstance(node, ast.FunctionDef):
                continue
            self.statement(node)

        # 模块级最后一条语句结束 → 隐式 return None
        self.module.emit('NONE', tree)
        self.module.emit('RETURN', tree)
        return self.build_program()

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

    def register_function(self, node):
        """只登记签名与槽位，不编译函数体。

        分两步的理由：编译函数体时会解析「调用哪个函数」，而 Python 允许
        「前面的方法调用后面的方法」。先登记全部签名，编译时才能查到。
        """
        if node.name in self.function_by_name:
            raise CompileError('函数 "%s" 定义了两次' % node.name, self.filename, node)
        if node.decorator_list:
            raise CompileError('不支持装饰器：脚本里没有一等函数', self.filename,
                               node.decorator_list[0])

        args = node.args
        if args.vararg or args.kwarg or args.kwonlyargs:
            raise CompileError('不支持 *args / **kwargs / 仅关键字参数', self.filename, node)
        # 位置-only 参数（`def f(a, /)`）同样拒绝：它在语法上分隔了「只按位置
        # 传」的参数，而这里根本没有关键字传参，接纳它只会让脚本看起来支持
        # 一个它并不区分的特性。
        if args.posonlyargs:
            raise CompileError('不支持位置-only 参数（/ 分隔符）',
                               self.filename, args.posonlyargs[0])

        params = [arg.arg for arg in args.args]
        defaults = list(args.defaults)
        if defaults:
            raise CompileError(
                '默认参数暂不支持字节码路线（AOT 路线支持字面量默认值）',
                self.filename, defaults[0])

        block = Block(node.name, len(params))
        for index, name in enumerate(params):
            block.locals[name] = index
        block.local_count = len(params)

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
        for child in ast.walk(node):
            if isinstance(child, ast.Global):
                declared_global.update(child.names)
        self_assigned = getattr(node, 'aipy_self_assigned', set())

        # 记下「预扫描登记的局部名」，供 global 检查区分「本就是局部」与
        # 「因为预扫描才进了 locals」：后者若同时是模块级变量，仍然是漏写
        # global 的笔误，该拒绝。
        preassigned = set()
        for child in ast.walk(node):
            if not isinstance(child, (ast.Assign, ast.AugAssign, ast.AnnAssign)):
                continue
            targets = child.targets if isinstance(child, ast.Assign) else [child.target]
            for target in targets:
                for name in self.target_names(target):
                    if name in declared_global or name in self_assigned:
                        continue
                    block.slot_of(name)
                    preassigned.add(name)
        block.preassigned = preassigned

        index = len(self.functions)
        self.function_by_name[node.name] = index
        self.functions.append({'block': block, 'node': node})

    def compile_function_body(self, node):
        """编译已经登记过的函数体。

        必须等**所有**函数登记完、**所有**全局槽占完再调用：前者让「前向调用」
        成立，后者让 global 检查能看见模块级变量。
        """
        entry = self.functions[self.function_by_name[node.name]]
        block = entry['block']

        # 函数里对模块级的赋值必须写 global：否则按 CPython 就是新建局部量，
        # 而那几乎总是笔误。与 pyuic 的 check_scope 同一条规则。
        #
        # 例外是通过 `self.x = ...` 来的名字：展开后它们与模块级变量同名，
        # 但原本写的是「改实例状态」，那是 Activity 的正当用法。
        self_assigned = getattr(node, 'aipy_self_assigned', set())
        declared_global = set()
        for child in ast.walk(node):
            if isinstance(child, ast.Global):
                declared_global.update(child.names)

        # 参数占前 param_count 个槽，它们是真正的局部量。
        params = set()
        for name, slot in block.locals.items():
            if slot < block.param_count:
                params.add(name)

        for child in ast.walk(node):
            if isinstance(child, (ast.Assign, ast.AugAssign, ast.AnnAssign)):
                targets = child.targets if isinstance(child, ast.Assign) else [child.target]
                for target in targets:
                    for name in self.target_names(target):
                        if name in self_assigned:
                            continue
                        # `name in block.locals` 不能直接用来豁免：预扫描会
                        # 把所有赋值目标都登记成局部，那样漏写 global 的笔误
                        # 就再也查不出来了。参数才是真正的局部量。
                        if name in self.globals and name not in declared_global \
                                and name not in params:
                            raise CompileError(
                                '函数里给模块级变量 %s 赋值，但没有写 global 声明。'
                                'CPython 会把它当成新的局部变量；要改模块级变量请先写 '
                                '`global %s`' % (name, name), self.filename, child)

        # 进入函数上下文编译函数体
        saved = self.block
        saved_loops = self.loop_stack
        saved_withs = self.with_stack
        self.block = block
        self.loop_stack = []
        self.with_stack = []
        for statement in node.body:
            self.statement(statement)
        self.with_stack = saved_withs
        self.loop_stack = saved_loops
        self.block = saved

        # 隐式 return None（并且保证 return 落点正确）
        block.emit('NONE', node)
        block.emit('RETURN', node)

    # ── 语句 ──

    def statement(self, node):
        if isinstance(node, ast.Expr):
            if isinstance(node.value, (ast.Yield, ast.YieldFrom)):
                raise CompileError('不支持 yield：脚本里没有生成器', self.filename, node.value)
            if isinstance(node.value, ast.Await):
                raise CompileError('不支持 await：脚本是单线程事件循环', self.filename, node.value)
            if isinstance(node.value, ast.Call):
                self.expr(node.value)
                self.block.emit('POP', node)
            return

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
            if node.orelse:
                raise CompileError('不支持 while ... else', self.filename, node)
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
            exit_at = len(self.block.code)
            self.block.patch_u16(exit_fixup, exit_at)
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
            raise CompileError('不支持 nonlocal；嵌套函数本身也不支持', self.filename, node)

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

        if isinstance(node, ast.FunctionDef):
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
        for item in node.items:
            if item.optional_vars is not None and not isinstance(item.optional_vars, ast.Name):
                raise CompileError('with ... as 后面只能跟一个名字',
                                   self.filename, item.optional_vars)

            # 管理器表达式先求值，WITH_ENTER 直接弹它。
            self.expr(item.context_expr)
            slot = self.block.alloc_slot()
            self.block.emit('WITH_ENTER', node)
            self.block.emit_u16(slot)

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

    def assign_target(self, target, node):
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
            raise CompileError('不支持元组解包赋值；请分开写', self.filename, target)
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
        if node.orelse:
            raise CompileError('不支持 for ... else', self.filename, node)
        if not isinstance(node.target, ast.Name):
            raise CompileError('for 的循环变量必须是单个名字', self.filename, node.target)

        self.expr(node.iter)
        self.block.emit('ITER_INIT', node)
        loop_top = len(self.block.code)
        self.block.emit('ITER_NEXT', node)
        exit_fixup = len(self.block.code)
        self.block.emit_u16(0)
        self.name_store(node.target.id, node)

        self.loop_stack.append((loop_top, [], True))
        for statement in node.body:
            self.statement(statement)
        _, breaks, _ = self.loop_stack.pop()

        self.block.emit('JUMP', node)
        self.block.emit_u16(loop_top)
        exit_at = len(self.block.code)
        self.block.patch_u16(exit_fixup, exit_at)
        for fixup in breaks:
            # break 时 ITER_BREAK 已经弹掉了循环状态，直接跳到出口。
            # 但 break 生成的是 ITER_BREAK + JUMP，JUMP 跳过的是 exit，
            # 而 exit 处不需要再弹栈，所以直接指向 exit_at 即可。
            self.block.patch_u16(fixup, exit_at)

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
            if node.id in BUILTIN_INDEX:
                # `len` 作为值出现时没有意义（没有一等函数），但 `len(x)` 是调用，
                # 走 call 分支。这里只处理它被当成普通名字读的情况。
                raise CompileError('内建函数 %s 不能当值使用' % node.id,
                                   self.filename, node)
            # 脚本函数同理：`g = f` 在这里会编译成「读一个叫 f 的全局变量」，
            # 而 f 从来没有被存进去过，运行期静默得到 None，直到调用 g() 才崩。
            # 报错比让它跑过去好——「函数是一等值」这件事本就不在子集里。
            if node.id in self.function_by_name:
                raise CompileError(
                    '函数 %s 不能当值使用（没有一等函数）；请直接调用它' % node.id,
                    self.filename, node)
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
            raise CompileError('不支持属性访问（没有对象模型）；扩展函数请用 '
                               '`模块.函数(...)` 调用', self.filename, node)

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
                    if part.format_spec is not None:
                        raise CompileError('f-string 里的格式说明符暂不支持',
                                           self.filename, node)
                    if part.conversion not in (-1, None):
                        raise CompileError('f-string 里的 !r / !s 转换暂不支持',
                                           self.filename, node)
                    self.expr(part.value)
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

    def call(self, node):
        # 内建：len/str/range… 编译成 CALL_BUILTIN + 下标。
        if isinstance(node.func, ast.Name) and node.func.id in BUILTIN_INDEX:
            if node.keywords:
                raise CompileError('不支持关键字传参；按位置传', self.filename, node)
            for argument in node.args:
                self.expr(argument)
            self.block.emit('CALL_BUILTIN', node)
            self.block.emit_u16(self.builtin_index(node.func.id))
            self.block.emit_u8(len(node.args))
            return

        # 脚本内函数：CALL + 函数号。
        if isinstance(node.func, ast.Name) and node.func.id in self.function_by_name:
            if node.keywords:
                raise CompileError('不支持关键字传参；按位置传', self.filename, node)
            for argument in node.args:
                self.expr(argument)
            self.block.emit('CALL', node)
            self.block.emit_u16(self.function_by_name[node.func.id])
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
            if node.keywords:
                raise CompileError('不支持关键字传参；按位置传', self.filename, node)
            # 接收者是扩展模块名时走 CALL_EXT；否则是方法调用。
            #
            # 模块名可以是点分的（`os.path.exists`）：取接收者的完整属性链，
            # 再判断它是不是 import 过的模块（含点分前缀匹配——`import os`
            # 之后 `os.path` 就可用，与 CPython 一致）。
            receiver_name = self.dotted_name(node.func.value)
            if receiver_name and self.is_module(receiver_name):
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
            for argument in node.args:
                self.expr(argument)
            self.block.emit('CALL_METHOD', node)
            self.block.emit_u16(self.const_string(node.func.attr))
            self.block.emit_u8(len(node.args))
            return

        raise CompileError('不支持这种调用目标', self.filename, node)

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
