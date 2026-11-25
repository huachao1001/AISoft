# 更硬的用例：嵌套循环、break/continue、字典、切片、递归、链式比较。
#
# 每一条都对应 VM 里一个容易写错的地方：跳转回填、栈的弹出顺序、
# 临时槽复用。这些错了不会崩溃，只会算错——所以必须逐条断言。

# ── 嵌套循环 + break / continue ──
found = 0
pairs = 0
for i in range(4):
    if i == 2:
        continue
    for j in range(4):
        pairs = pairs + 1
        if j == 1:
            break
        found = found + 1

# ── 字典 ──
table = {"a": 1, "b": 2}
table["c"] = 3
table["a"] = table["a"] + 10
keys = table.keys()
has_b = "b" in table
value_c = table.get("c")
missing = table.get("z", -1)

# ── 字符串切片 ──
text = "abcdef"
head = text[0:3]
tail = text[3:]
every_other = text[0:6:2]
char = text[1]

# ── 列表方法 ──
items = [3, 1, 2]
items.append(4)
items.sort()
first = items[0]
last = items.pop()
count_after_pop = len(items)

# ── 递归 ──
def factorial(n):
    if n <= 1:
        return 1
    return n * factorial(n - 1)


fact5 = factorial(5)

# ── 链式比较 ──
x = 5
in_range = 1 < x < 10
not_in_range = 1 < x < 3
simple = x == 5

# ── 负下标与负数运算 ──
numbers = [10, 20, 30]
negative = numbers[-1]
floored = -7 // 2
modulo = -7 % 2

# ── 逻辑运算返回操作数本身 ──
fallback = "" or "default"
keep = "value" or "other"
both = 1 and 2

# ── 字典遍历 ──
total_keys = 0
for key in table:
    total_keys = total_keys + 1
