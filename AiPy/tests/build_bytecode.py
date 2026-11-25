"""生成 test_vm 需要的字节码，并检查编译器与 VM 的操作码表一致。

这个脚本是「两套语言实现之间唯一的契约检查」。操作码编号在
`tools/pcc/src/codegen/tables.cpp` 的表里、在 `vm.h` 的 enum class 里
各有一份，顺序错一个就是「所有指令集体错位」，而症状是「程序跑出莫名其妙的
结果」，不是崩溃。所以必须自动对拍。

字节码由 `pcc.exe` 生成——它是发布件本身，用它生成测试输入等于顺带
验证「编译器产出的字节码 VM 能读」。

用法：build_bytecode.py <pcc.exe> --output-dir <dir>
"""

import io
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AIPY = os.path.normpath(os.path.join(HERE, '..'))
TABLES = os.path.join(AIPY, 'tools', 'pcc', 'src', 'codegen', 'tables.cpp')


def opcodes_from_header():
    """从 AiPy/include/aipy/vm.h 抽出 Op 枚举的顺序。"""
    path = os.path.join(AIPY, 'include', 'aipy', 'vm.h')
    with io.open(path, 'r', encoding='utf-8') as handle:
        text = handle.read()
    match = re.search(r'enum class Op : std::uint8_t \{(.*?)\};', text, re.DOTALL)
    if not match:
        raise SystemExit('vm.h 里找不到 Op 枚举')
    body = match.group(1)
    # 去掉行注释，再按逗号切成名字
    body = re.sub(r'//[^\n]*', '', body)
    names = []
    for item in body.split(','):
        item = item.strip()
        if not item:
            continue
        name = item.split('=')[0].strip()
        names.append(name)
    return names


def table_names(source, table):
    """从 tables.cpp 的 `const char* const k<Table>[] = {...}` 取字符串。"""
    path = os.path.join(AIPY, 'tools', 'pcc', 'src', 'codegen', 'tables.cpp')
    with io.open(path, 'r', encoding='utf-8') as handle:
        text = handle.read()
    match = re.search(r'const char\* const %s\[\] = \{(.*?)\};' % table,
                      text, re.DOTALL)
    if not match:
        raise SystemExit('tables.cpp 里找不到 %s' % table)
    # 注释里也可能有引号，先去掉注释行。
    body = re.sub(r'//[^\n]*', '', match.group(1))
    return re.findall(r'"([^"]*)"', body)


def normalize(name):
    """kLoadLocal → LOAD_LOCAL，用来与 C++ 侧的名字对齐。"""
    if name.startswith('k'):
        name = name[1:]
    return re.sub(r'(?<!^)(?=[A-Z])', '_', name).upper()


def check_opcodes():
    header = [normalize(name) for name in opcodes_from_header()]
    names = table_names(TABLES, 'kOpNames')
    if header == names:
        return []
    problems = []
    for index in range(max(len(header), len(names))):
        h = header[index] if index < len(header) else '<缺失>'
        c = names[index] if index < len(names) else '<缺失>'
        if h != c:
            problems.append('  第 %d 条：vm.h=%s  tables.cpp=%s' % (index, h, c))
    return ['操作码表不一致：'] + problems


def check_tables():
    problems = []
    problems.extend(check_opcodes())

    # 内建表：编译器的名字与 AiPy 的 kBuiltinTable 顺序必须一致。
    path = os.path.join(AIPY, 'src', 'builtins.cpp')
    with io.open(path, 'r', encoding='utf-8') as handle:
        text = handle.read()
    match = re.search(r'kBuiltinTable\[\] = \{(.*?)\};', text, re.DOTALL)
    if not match:
        problems.append('builtins.cpp 里找不到 kBuiltinTable')
        return problems
    names = re.findall(r'\{"([^"]+)"', match.group(1))
    compiled = table_names(TABLES, 'kBuiltins')
    if names != compiled:
        problems.append('内建表不一致：')
        problems.append('  AiPy      : %s' % ', '.join(names))
        problems.append('  编译器    : %s' % ', '.join(compiled))
    return problems


def generate(pcc, script, output):
    completed = subprocess.run([pcc, script, output], capture_output=True,
                               text=True, encoding='utf-8', errors='replace')
    if completed.returncode != 0:
        raise SystemExit('编译 %s 失败：%s'
                         % (os.path.basename(script), completed.stderr.strip()))
    print('  %-28s %6d 字节' % (os.path.basename(output),
                                os.path.getsize(output)))


def main(argv):
    if len(argv) < 2:
        print('用法: build_bytecode.py <pcc.exe> [--output-dir <dir>]',
              file=sys.stderr)
        return 2
    # argv 是「去掉脚本名之后」的参数，所以第一个就是 pcc。
    # 写成 argv[1] 会把 "--output-dir" 当编译器路径，症状是
    # FileNotFoundError: 系统找不到指定的文件。
    pcc = os.path.abspath(argv[0])

    output_dir = None
    index = 1
    while index < len(argv):
        if argv[index] == '--output-dir' and index + 1 < len(argv):
            output_dir = argv[index + 1]
            index += 2
            continue
        index += 1

    problems = check_tables()
    if problems:
        for line in problems:
            print(line, file=sys.stderr)
        return 1

    # 没有 --output-dir 就写到临时目录：字节码是派生物，默认不落在
    # 源码树里，否则「改了脚本忘记重新生成」会变成假失败。
    out_dir = output_dir or os.path.join(tempfile.gettempdir(), 'aipy_bytecode')
    os.makedirs(out_dir, exist_ok=True)
    scripts_dir = os.path.join(HERE, 'scripts')
    count = 0
    for name in sorted(os.listdir(scripts_dir)):
        if not name.endswith('.py'):
            continue
        generate(pcc, os.path.join(scripts_dir, name),
                 os.path.join(out_dir, name[:-3] + '.bc'))
        count += 1
    print('操作码与内建表一致；生成了 %d 份字节码 -> %s' % (count, out_dir))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

