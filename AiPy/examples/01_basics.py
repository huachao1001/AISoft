# 示例 1：基础语法。
#
# 覆盖：变量、算术、字符串、条件、while/for、函数、列表、字典、切片。
# 这份脚本也能被 CPython 直接执行（`python hello.py`），两边的输出应当
# 一致——它是「AiPy 实现的是 Python 子集」这句话的最小证据。

name = "AiPy"
version = 1

greeting = name + " v" + str(version)
print(greeting)

# ── 算术 ──
# Python 的整数除法是向下取整，负数也一样：-7 // 2 == -4 而不是 -3。
print("7 / 2 =", 7 / 2)
print("7 // 2 =", 7 // 2)
print("-7 // 2 =", -7 // 2)
print("-7 % 2 =", -7 % 2)
print("2 ** 10 =", 2 ** 10)

# ── 条件 ──
score = 83
if score >= 90:
    grade = "A"
elif score >= 80:
    grade = "B"
else:
    grade = "C"
print("score", score, "->", grade)

# ── while ──
total = 0
counter = 1
while counter <= 100:
    total = total + counter
    counter += 1
print("1..100 累加 =", total)

# ── 函数 ──
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


print("fib(10) =", fib(10))

# ── 列表 ──
numbers = [5, 3, 8, 1, 9, 2]
numbers.sort()
print("排序后 =", numbers)
print("前三个 =", numbers[0:3])
print("倒序 =", numbers[::-1])
print("最大值 =", max(numbers), "最小值 =", min(numbers), "和 =", sum(numbers))

# ── 字典 ──
ages = {"alice": 30, "bob": 25}
ages["carol"] = 35
print("carol 的年龄 =", ages["carol"])
print("dave 的年龄 =", ages.get("dave", -1))
# 注意这里没有直接 print(ages.keys())：
# CPython 得到 `dict_keys([...])` 视图对象，而 AiPy 的 keys() 返回普通列表。
# 这是容器语义的已知差异（SUPPORT.md B6「容器没有 dict 之外的变体」）。
# 示例只展示两边一致的部分：要键列表就显式收集。
key_list = []
for key in ages:
    key_list.append(key)
print("键 =", key_list)

# ── 字符串 ──
text = "  Hello, AiPy!  "
print("strip = [" + text.strip() + "]")
print("upper =", text.strip().upper())
print("split =", "a,b,c".split(","))
print("join =", "-".join(["x", "y", "z"]))
print("replace =", "abcabc".replace("ab", "X"))
print("find =", "abcdef".find("cd"))

# ── 遍历字典 ──
count = 0
for key in ages:
    count = count + 1
print("字典有", count, "个键")

# ── 中文按字符处理 ──
chinese = "你好世界"
print("字符数 =", len(chinese))
print("前两个字 =", chinese[0:2])
for ch in chinese:
    print("  字:", ch)
