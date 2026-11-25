# 示例 6：文件、目录、时间、JSON —— 四个系统库一起用。
#
# 这份脚本演示 `libs/fs`、`libs/os`、`libs/os_path`、`libs/time`、
# `libs/json`。它们和 `threading` 一样是**独立静态库**，按脚本实际调用到的
# 函数链接：
#
#   - 只用 open()，不会把 os.listdir 的目录遍历拖进产物
#   - 只用 os.path.exists，不会把 os.makedirs 拖进产物
#
#   dist\pcc.exe --exe examples\06_files.py demo.exe ^
#       --build-dir build --lib-dir build
#
# ── 与 CPython 的对应关系 ──
#
# 接口按标准 Python 对齐：
#
#   CPython                          AiPy
#   ────────────────────────────     ────────────────────────────────
#   open(p)                          open(p)          ← 裸名，就是 fs.open
#   os.path.exists(p)                os.path.exists(p)
#   os.path.getsize(p)               os.path.getsize(p)
#   os.listdir(p)                    os.listdir(p)
#   os.makedirs(p, exist_ok=True)    os.makedirs(p)
#   os.remove(p)                     os.remove(p)
#   time.time()                      time.time()
#   json.dumps(x)                    json.dumps(x)
#
# `open` 是 Python 的**内建**，不是模块函数——所以这里也写成裸的 open()。
# 它由 fs 这个 pylib 用 AIPY_PYLIB_BUILTIN 注册成短名，脚本不需要 import fs。
#
# 两处语言层面的差异：属性变方法（f.closed() 而不是 f.closed），失败返回
# None/False 而不是抛异常——见 LANGUAGE.md。
#
# 这份示例不与 CPython 逐行比对（见 tests/check_examples.py），而是断言关键
# 输出。理由：文件系统操作的输出依赖机器（路径、目录顺序），逐行比对会 flaky。

import json
import os
import os.path
import time

# 在脚本自己的目录下建工作区。用相对路径：打包成 exe 之后，
# 当前目录就是用户启动它的地方。
DIR = "aipy_demo_data"

# ── 1. 目录与写入 ──
# os.makedirs 对已存在的目录返回 True，所以重复运行不会失败。
print("建目录: " + str(os.makedirs(DIR)))

records_path = DIR + "/records.txt"

# open 是内建，用法与 CPython 一致：with 退出时一定关闭（包括循环里 break）。
with open(records_path, "w") as handle:
    handle.write("alpha\nbeta\ngamma\n")

print("写文件: " + str(os.path.exists(records_path)))

# ── 2. 读回来并处理 ──
with open(records_path) as handle:
    text = handle.read()

lines = text.split("\n")

# 末尾的换行会切出一个空串，去掉它。
names = []
for line in lines:
    if len(line) > 0:
        names.append(line.upper())

print("记录数: " + str(len(names)))
print("内容: " + str(names))

# ── 3. 追加 ──
with open(records_path, "a") as handle:
    handle.write("delta\n")
print("追加后文件非空: " + str(os.path.getsize(records_path) > 0))

# ── 4. JSON：把列表存成结构化数据 ──
payload = {"names": names, "count": len(names), "ok": True}
encoded = json.dumps(payload)
print("JSON: " + encoded)

# 读回来并验证类型：count 应当是整数而不是字符串。
decoded = json.loads(encoded)
print("还原 count: " + str(decoded["count"]))
print("还原 names: " + str(decoded["names"]))

# ── 5. 时间 ──
start = time.monotonic()
time.sleep(0.01)
elapsed = time.monotonic() - start

# 只断言「确实等了、时间在往前走」，不断言具体数值——那会 flaky。
print("计时有效: " + str(elapsed > 0.0))

stamp = time.time()
print("时间是数字: " + str(stamp > 1600000000.0))

# 格式化：只断言年份额，不比对完整字符串（那是机器相关的）。
year = time.strftime("%Y")
print("年份正常: " + str(len(year) == 4))

# ── 6. with 文件对象 ──
lines_path = DIR + "/lines.txt"
with open(lines_path, "w") as handle:
    handle.write("第一行\n")
    handle.write("第二行\n")

print("with 写完: " + str(os.path.getsize(lines_path) > 0))

# `open` 对齐 CPython 的文件对象：read/readline/readlines/write/writelines/
# seek/tell/flush/close 同名同义，`for line in f` 也能用。
with open(lines_path) as handle:
    total = 0
    for line in handle:
        total += len(line)

print("逐行读完共 " + str(total) + " 字")

# ── 7. 列出目录，确认文件都在 ──
entries = os.listdir(DIR)
entries.sort()
print("目录内容: " + str(entries))

# ── 8. 清理，让重复运行保持干净 ──
# 分开两次调用：`and` 的右操作数是**函数返回值**（布尔），
# 写成 `print(str(a and b))` 时 a 是真值会让它去求 b，而 str(b) 里 b
# 又是布尔——两层嵌套容易看错，分开写最清楚。
removed_records = os.remove(records_path)
removed_lines = os.remove(lines_path)
print("清理: " + str(removed_records and removed_lines))
print("清理后存在: " + str(os.path.exists(records_path)))
