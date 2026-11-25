# 示例 6：文件、时间、JSON —— 三个系统库一起用。
#
# 这份脚本演示 `libs/fs`、`libs/time`、`libs/json`。它们和 `threading`
# 一样是**独立静态库**，按脚本实际调用到的函数链接：
#
#   - 只用 fs.read_text，fs.open 的文件对象代码不进产物
#   - 只用 json.dumps/loads，不需要别的
#
#   python tools\aipyic\aipyic.py --exe examples\06_files.py demo.exe ^
#       --build-dir build --lib-dir build
#
# ── 与 CPython 的差别 ──
#
# `fs` / `json` / `time` 在 CPython 里都有同名模块，但接口形状不同：
#
#   CPython                          AiPy
#   ────────────────────────────     ────────────────────────────────
#   open(p).read()                   fs.read_text(p)
#   open(p, "w").write(s)            fs.write_text(p, s)
#   os.path.exists(p)                fs.exists(p)
#   os.listdir(p)                    fs.listdir(p)
#   time.time()                      time.time()
#   json.dumps(x)                    json.dumps(x)
#
# 所以这份示例不与 CPython 逐行比对（见 tests/check_examples.py），
# 而是断言关键输出。选这个示例的理由：它是最接近「真实脚本」的那一份
# ——读输入、算、写结果，而不是只做算术。

import fs
import json
import time

# 在脚本自己的目录下建工作区。用相对路径：打包成 exe 之后，
# 当前目录就是用户启动它的地方。
DIR = "aipy_demo_data"

# ── 1. 目录与写入 ──
# mkdir 对已存在的目录返回 True（相当于 makedirs(exist_ok=True)），
# 所以重复运行不会失败。
print("建目录: " + str(fs.mkdir(DIR)))

# 拼路径用手写的分隔符：AiPy 没有 os.path.join。正斜杠在 Windows 上
# 也能用，而且是跨平台的写法。
records_path = DIR + "/records.txt"
print("写文件: " + str(fs.write_text(records_path, "alpha\nbeta\ngamma\n")))

# ── 2. 读回来并处理 ──
text = fs.read_text(records_path)
lines = text.split("\n")

# 末尾的换行会切出一个空串，去掉它。
names = []
for line in lines:
    if len(line) > 0:
        names.append(line.upper())

print("记录数: " + str(len(names)))
print("内容: " + str(names))

# ── 3. 追加 ──
fs.append_text(records_path, "delta\n")
print("追加后行数: " + str(fs.size(records_path) > 0))

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
# 退出时一定调 __exit__ 关闭文件，包括循环里 break——与锁的语义一致。
lines_path = DIR + "/lines.txt"
with fs.open(lines_path, "w") as handle:
    handle.write("第一行\n")
    handle.write("第二行\n")

print("with 写完: " + str(fs.size(lines_path) > 0))

# `open` 对齐 CPython 的文件对象：read/readline/readlines/write/writelines/
# seek/tell/flush/close 同名同义，`for line in f` 也能用。
# 两处语言层面的差异：属性变方法（f.closed() 而不是 f.closed），
# 失败返回 None 而不是抛异常。
with fs.open(lines_path) as handle:
    total = 0
    for line in handle:
        total += len(line)

print("逐行读完共 " + str(total) + " 字")

# 只读文本文件，不发明「精确字节数」这种机器相关的断言。
back = fs.read_text(lines_path)
print("with 读回行数: " + str(len(back.split("\n")) - 1))

# ── 7. 列出目录，确认文件都在 ──
entries = fs.listdir(DIR)
entries.sort()
print("目录内容: " + str(entries))

# ── 8. 清理，让重复运行保持干净 ──
# 分开两次调用：`and` 的右操作数是**函数返回值**（布尔），
# 写成 `print(str(a and b))` 时 a 是真值会让它去求 b，而 str(b) 里 b
# 又是布尔——两层嵌套容易看错，分开写最清楚。
removed_records = fs.remove(records_path)
removed_lines = fs.remove(lines_path)
print("清理: " + str(removed_records and removed_lines))
print("清理后存在: " + str(fs.exists(records_path)))
