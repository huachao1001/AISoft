# 示例 2：一个能真正干活的小工具——文本词频统计。
#
# 这份脚本用到的都是「写脚本时真的会需要」的东西：读一行输入的模拟数据、
# 切分、字典累加、排序输出。它同时演示了 AiPy 支持的字符串与字典方法。
#
# 为什么不用 f-string：语言子集里没有（见 docs/SUPPORT.md 的 C 类）。
# 拼接用 `+` 和 `str()`，这样同一份脚本在 CPython 里也能跑。

CORPUS = """\
the quick brown fox jumps over the lazy dog
the dog barks and the fox runs
a quick brown dog outpaces a quick fox
"""

# ── 切词 ──
# split() 在 Python 里不带参数表示「按任意空白切」，但 AiPy 的 split 需要
# 显式分隔符（语言子集边界）。所以先手动把换行与连续空格规范化。
normalized = CORPUS.replace("\n", " ")
while "  " in normalized:
    normalized = normalized.replace("  ", " ")

words = normalized.strip().split(" ")

# ── 累加词频 ──
counts = {}
for word in words:
    if word in counts:
        counts[word] = counts[word] + 1
    else:
        counts[word] = 1

print("总词数:", len(words))
print("不同词:", len(counts.keys()))

# ── 找出最高频的词 ──
# 这里刻意不用「排序后取第一个」：字典遍历顺序在 CPython 3.7+ 是插入序，
# 但用「打擂台」找最大值不依赖任何顺序保证，行为更稳。
best_word = ""
best_count = 0
for word in counts:
    if counts[word] > best_count:
        best_count = counts[word]
        best_word = word

print("最高频:", best_word, "出现", best_count, "次")

# ── 按词长分组 ──
by_length = {}
for word in counts.keys():
    size = len(word)
    if size not in by_length:
        by_length[size] = []
    by_length[size].append(word)

print("按长度分组:")
for size in by_length:
    print("  ", size, "个字母:", by_length[size])

# ── 出报告 ──
def repeat(text, times):
    result = ""
    for i in range(times):
        result = result + text
    return result


print("")
print("频率柱状图:")
for word in counts:
    bar = repeat("*", counts[word])
    print("  " + word + " " + bar)
