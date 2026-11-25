# 示例 4：一个能打包成 exe 交付的小工具。
#
# 这份脚本演示的是**交付形态**：写完之后执行
#
#   dist\pcc.exe --exe examples\04_tool.py build\invoice.exe
#
# 就得到一个独立 exe。用户机器上不需要 Python、不需要 MSVC、不需要链接器。
#
# ── 为什么这个示例不读输入 ──
#
# 它演示的是「固定规则 + 批量数据」，所以数据写在脚本里。要交互的写法见
# `07_interactive.py`：`input()` 来自 fs.pylib 的裸名注册，与 `open` 同构。
# 两者是不同的交付形态——这份算一张固定的结算单，那份问几个问题再算。
#
# ── 它算的是什么 ──
#
# 一份订单的优惠结算：满减、折扣、税额、分档运费。规则足够琐碎，正好
# 说明为什么会想把它从主程序里拆出来——改一次规则不该重新编译整个程序。

ORDERS = [
    {"name": "机械键盘", "price": 459, "count": 1},
    {"name": "显示器支架", "price": 129, "count": 2},
    {"name": "USB 集线器", "price": 89, "count": 3},
    {"name": "键盘腕托", "price": 45, "count": 1},
]

# 优惠规则。改这里不需要碰任何 C++。
DISCOUNT_THRESHOLD = 800   # 满这个金额才打折
DISCOUNT_RATE = 0.9
SHIPPING_FREE_AT = 500
SHIPPING_FEE = 12
TAX_RATE = 0.13


def line_total(order):
    return order["price"] * order["count"]


def money(value):
    # 保留两位小数。没有 f-string，所以用 round 再补零。
    rounded = round(value, 2)
    text = str(rounded)
    if "." not in text:
        return text + ".00"
    decimals = len(text) - text.find(".") - 1
    while decimals < 2:
        text = text + "0"
        decimals = decimals + 1
    return text


print("=" * 46)
print("订单结算单")
print("=" * 46)

subtotal = 0
item_count = 0
for order in ORDERS:
    total = line_total(order)
    subtotal = subtotal + total
    item_count = item_count + order["count"]
    print("  " + order["name"] + " x" + str(order["count"])
          + "  " + money(total))

print("-" * 46)
print("小计        " + money(subtotal) + " 元（" + str(item_count) + " 件）")

# ── 折扣 ──
discount = 0
if subtotal >= DISCOUNT_THRESHOLD:
    discount = round(subtotal * (1 - DISCOUNT_RATE), 2)
    print("满 " + str(DISCOUNT_THRESHOLD) + " 减"
          + "  -" + money(discount))
else:
    print("未满 " + str(DISCOUNT_THRESHOLD) + "，无折扣")

after_discount = round(subtotal - discount, 2)

# ── 运费 ──
shipping = 0
if after_discount < SHIPPING_FREE_AT:
    shipping = SHIPPING_FEE
    print("运费        " + money(shipping) + " 元")
else:
    print("运费        免运费")

# ── 税额 ──
taxable = round(after_discount + shipping, 2)
tax = round(taxable * TAX_RATE, 2)
print("税额(" + str(int(TAX_RATE * 100)) + "%)  " + money(tax) + " 元")

total_payable = round(taxable + tax, 2)
print("=" * 46)
print("应付        " + money(total_payable) + " 元")
print("=" * 46)

# ── 自检：脚本自己核对算没算错 ──
# 打包成 exe 之后没人会读源码，所以关键结果要能自己验。
expected_subtotal = 459 + 129 * 2 + 89 * 3 + 45
if subtotal != expected_subtotal:
    print("自检失败：小计应当是 " + str(expected_subtotal))

if total_payable <= 0:
    print("自检失败：应付金额不可能是 " + money(total_payable))
