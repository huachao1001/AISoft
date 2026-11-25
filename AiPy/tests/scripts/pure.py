# 纯语言脚本：不碰任何界面 API，用来验证 VM 的端到端。

total = 0
items = [1, 2, 3, 4, 5]
for x in items:
    total = total + x

names = ["a", "b"]
joined = "-".join(names)
upper_text = "hello".upper()

text = ""
for i in range(3):
    text = text + str(i)

counter = 0
while counter < 3:
    counter += 1

label = "big" if total > 10 else "small"


def double(value):
    return value * 2


def add_all(a, b, c):
    return a + b + c


doubled = double(21)
summed = add_all(1, 2, 3)
length = len(items)
maximum = max(items)
minimum = min(items)
rounded = round(3.14159, 2)
