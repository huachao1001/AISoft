# 示例 3：内建函数与数值处理。
#
# 覆盖 AiPy 的 17 个内建里偏「实用工具」的那部分。其中两个不是 Python
# 内建，是 AiPy 为界面脚本加的（见 docs/SUPPORT.md）：
#
#   format_text      单占位符格式化（`{}`），避免 f-string
#   char_from_code   码点 → 字符（配合 text_of_char 做字符处理）
#
# 文本转数字用标准 Python 的 int() / float()：失败抛 ValueError，与 CPython
# 完全一致，脚本里用 try/except 兜住。

# ── 数值内建 ──
values = [3.7, -2.4, 8.1, -0.5, 12.9]
print("绝对值 =", abs(-42), abs(-3.5))
print("四舍五入 =", round(3.14159, 2), round(2.5), round(-2.5))
print("最小值 =", min(values), "最大值 =", max(values))
print("整数和 =", sum([1, 2, 3, 4, 5]))

# Python 的 round 是「银行家舍入」：round(2.5) == 2 而不是 3。
print("注意 round(2.5) =", round(2.5), "（银行家舍入）")

# ── 类型转换 ──
print("str(42) =", str(42))
print("int('17') =", int("17"))
print("int(3.99) =", int(3.99))
print("float('2.5') =", float("2.5"))
print("bool(0) =", bool(0), "bool(1) =", bool(1))

# ── 转换失败抛 ValueError，用 try/except 兜住 ──
# 这是与 CPython 完全一致的写法：迁移回去不用改。
good = int("42")
print("int('42') =", good)
try:
    bad = int("not a number")
    print("这行不会执行")
except ValueError as error:
    print("int 解析失败:", error)

try:
    float("abc")
except ValueError as error:
    print("float 解析失败:", error)

# ── 数值累加与精度 ──
# 浮点求和会累积误差，这是 IEEE 754 的性质，CPython 也一样。
total = 0.0
for i in range(10):
    total = total + 0.1
print("十次加 0.1 =", total)
print("round 到两位 =", round(total, 2))

# ── 整数除法的两种语义 ──
# 每个元素是 [被除数, 除数] 的列表，不是元组——语言子集里元组统一当列表。
pairs = [[7, 2], [-7, 2], [7, -2], [-7, -2]]
for pair in pairs:
    a = pair[0]
    b = pair[1]
    print(str(a) + " // " + str(b) + " = " + str(a // b),
          " " + str(a) + " % " + str(b) + " = " + str(a % b))

# ── 除零抛 ZeroDivisionError ──
try:
    print(1 / 0)
except ZeroDivisionError as error:
    print("除零:", error)

# ── format_text ──
# 第二个参数是**序列**（按 iterate 展开），不是单个值：`format_text(模板, 列表)`。
# 传一个字符串会被当成字符序列逐字填进去——这是最容易踩的一处。
template = "坐标 ({}, {})"
print(format_text(template, [10, 20]))
print(format_text("{} + {} = {}", [1, 2, 3]))

# ── 位运算 ──
print("5 & 3 =", 5 & 3)
print("5 | 3 =", 5 | 3)
print("5 ^ 3 =", 5 ^ 3)
print("1 << 10 =", 1 << 10)
print("1024 >> 3 =", 1024 >> 3)
print("~5 =", ~5)

# ── range 的三种形式 ──
print("range(5) =", range(5))
print("range(2, 8) =", range(2, 8))
print("range(0, 10, 3) =", range(0, 10, 3))
print("range(5, 0, -1) =", range(5, 0, -1))

# ── 用 range 反向遍历（没有 reversed 内建）──
countdown = []
for i in range(5, 0, -1):
    countdown.append(i)
print("倒计数 =", countdown)

# ── 断言式自检：脚本自己判断结果对不对 ──
checks = []
checks.append(round(3.14159, 2) == 3.14)
checks.append(abs(-42) == 42)
checks.append(sum([1, 2, 3]) == 6)
checks.append(int(3.99) == 3)
try:
    int("x")
    checks.append(False)
except ValueError:
    checks.append(True)

passed = 0
for ok in checks:
    if ok:
        passed = passed + 1
print("自检通过:", passed, "/", len(checks))
