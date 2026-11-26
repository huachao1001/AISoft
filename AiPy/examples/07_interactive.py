# 示例 7：交互式程序——双击后等着你输入。
#
# 这份示例演示 `input()`。它和 `open` 一样是**裸名**：实现来自 fs.pylib，
# 脚本里不用 import 就能用。双击打包好的 exe，你会看到提示语停在窗口里，
# 光标在等键盘输入；敲完回车程序才继续。
#
# ── 与 CPython 的差异（只剩这一处） ──
#
# stdin 到底时（管道里没数据了）CPython 抛 EOFError，这里同样抛。
# `aipy_input` 拿 CPython 逐字节对拍守着这条。
#
# ── 为什么值得一个示例 ──
#
# 没有它，「打包出来的 exe 是不是只能跑一遍固定逻辑」就没有答案。带上
# input 之后，打包产物可以是真正的小工具：问几个问题，算个结果。

# 没有 f-string，所以金额要自己补两位小数。`round` 返回浮点数，
# 整数会被 `str()` 成 "195000.0"——这个函数把它规范成 "195000.00"。
def money(value):
    rounded = round(value, 2)
    text = str(rounded)
    if "." not in text:
        return text + ".00"
    decimals = len(text) - text.find(".") - 1
    while decimals < 2:
        text = text + "0"
        decimals = decimals + 1
    return text


print("=== 个税试算 ===")
print("")

# input 的提示语不带换行，光标紧跟在冒号后面——与 CPython 一致。
name = input("你的名字: ")
income = input("月收入（元）: ")
months = input("发几个月: ")

# 输入永远是字符串，要算数得自己转。int()/float() 失败抛 ValueError，
# 这里用 try/except 兜住——与 CPython 写法完全一致。
try:
    salary = float(income)
    bonus_months = int(months)
except ValueError:
    salary = None
    bonus_months = None

if salary == None or bonus_months == None:
    print("")
    print("收入与月数都必须是数字，收到的是：" + income + " / " + months)
else:
    # 起征点 5000，超出部分按 10% 简化计算（示例只演示流程，不是税法）。
    total = salary * bonus_months
    taxable = total - 5000 * bonus_months
    if taxable < 0:
        taxable = 0
    tax = round(taxable * 0.1, 2)

    print("")
    print(name + " 的年收入 " + money(total) + " 元")
    print("  应纳税所得额 " + money(taxable) + " 元")
    print("  简化个税     " + money(tax) + " 元")
    print("  到手         " + money(total - tax) + " 元")

print("")
print("（直接回车结束）")
input()
