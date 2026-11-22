# 计算器：Python 脚本驱动界面
#
# 这个文件只写「行为」：按键做什么、算式怎么算、键盘怎么响应。
# 界面长什么样全在 assets/layouts/calculator.xml 里，两边的连接点只有 id。
#
# 构建期 pyuic 会把这份脚本编译成 C++ 类，所以运行时没有解释器：
# 每个方法都变成 C++ 成员函数，每个 self.字段都变成成员变量。
#
# ── 写法对齐 Android ──
# 入口界面是 class CalculatorActivity(Activity)，onCreate 里做绑定。
# 状态写成 self.xxx：字段属于这个实例，方法里改它不用写 global。

# ── 窗口描述 ──
# 窗口的标题、尺寸、布局、主题全部写在同目录的 manifest.xml 里。
# 这里不再有 TITLE / WIDTH / HEIGHT：那些是**数据**，不是行为。
# 改窗口大小只改清单，不会让这份脚本重新生成。
#
# 两处都写同一个字段会在构建期报错，所以每个字段只有一个来源。


class CalculatorActivity(Activity):
    # ── 状态 ──
    # 类体里的赋值就是字段初值，等价于在 __init__ 里写 self.xxx = ...。
    entry = "0"        # 当前正在输入的数字
    pending = None     # 尚未应用的运算符
    accumulated = 0.0  # 已经算出的左操作数
    fresh = True       # 下次输入数字是否要清空显示

    # ── 显示 ──
    # 所有改显示的地方都走 show()：状态与画面保持一个方向，
    # 否则总有一处忘记同步，表现为「按了键但屏幕没变」。

    def show(self):
        set_text(find("display"), self.entry)

    def show_history(self, text):
        set_text(find("history"), text)

    # ── 数字格式化 ──
    # 直接 str(1.0 / 3.0) 会得到很长一串；计算器要的是能读的 8 位。

    def format_number(self, number):
        # 整数值不显示小数点：输入 3 再按 = 不该变成 3.0
        if number == float(int(number)):
            return str(int(number))
        return str(round(number * 100000000.0) / 100000000.0)

    # ── 输入 ──

    def press_digit(self, digit):
        if self.fresh:
            self.entry = digit
            self.fresh = False
        elif self.entry == "0":
            self.entry = digit
        else:
            self.entry = self.entry + digit
        # 超过 12 位就拒绝继续输入，而不是存进去再靠省略号截断
        if len(self.entry) > 12:
            self.entry = self.entry[:12]
        self.show()

    def press_dot(self):
        if self.fresh:
            self.entry = "0."
            self.fresh = False
        elif "." not in self.entry:
            self.entry = self.entry + "."
        self.show()

    def press_operator(self, op):
        # 连按运算符时以前一个为准：3 + × 5 应该是 3 × 5
        if self.pending is not None and not self.fresh:
            self.apply_pending()
        self.accumulated = float(self.entry)
        self.pending = op
        self.fresh = True
        self.show_history(self.format_number(self.accumulated) + " " + op)

    def apply_pending(self):
        if self.pending is None:
            return
        right = float(self.entry)
        if self.pending == "+":
            self.accumulated = self.accumulated + right
        elif self.pending == "-":
            self.accumulated = self.accumulated - right
        elif self.pending == "×":
            self.accumulated = self.accumulated * right
        elif self.pending == "÷":
            if right == 0.0:
                # 除零不给 inf：显示「无法除以 0」比显示 inf 有用
                self.pending = None
                self.entry = "0"
                self.show()
                return
            self.accumulated = self.accumulated / right
        self.pending = None

    def press_equals(self):
        if self.pending is None:
            return
        expression = (self.format_number(self.accumulated) + " " + self.pending +
                      " " + self.format_number(float(self.entry)))
        self.apply_pending()
        self.entry = self.format_number(self.accumulated)
        self.fresh = True
        self.show()
        self.show_history(expression + " =")

    def press_clear(self):
        self.entry = "0"
        self.pending = None
        self.accumulated = 0.0
        self.fresh = True
        self.show()
        self.show_history("")

    def press_back(self):
        if self.fresh or len(self.entry) <= 1:
            self.entry = "0"
            self.fresh = True
        else:
            self.entry = self.entry[:len(self.entry) - 1]
        self.show()

    def press_sign(self):
        if self.entry != "0":
            if self.entry[0] == "-":
                self.entry = self.entry[1:]
            else:
                self.entry = "-" + self.entry
        self.show()

    def press_percent(self):
        self.entry = self.format_number(float(self.entry) / 100.0)
        self.fresh = True
        self.show()

    # ── 键盘 ──
    # 鼠标点按钮之外，键盘也能用。字符走 on_char（宿主在视图树没消费后转进来）。

    def on_char(self, codepoint):
        ch = char_from_code(codepoint)
        if ch >= "0" and ch <= "9":
            self.press_digit(ch)
            return True
        if ch == ".":
            self.press_dot()
            return True
        if ch == "+":
            self.press_operator("+")
            return True
        if ch == "-":
            self.press_operator("-")
            return True
        if ch == "*":
            self.press_operator("×")
            return True
        if ch == "/":
            self.press_operator("÷")
            return True
        if ch == "=":
            self.press_equals()
            return True
        if ch == "%":
            self.press_percent()
            return True
        return False

    def on_key(self, key, mods):
        if key == "Enter":
            self.press_equals()
            return True
        if key == "Escape":
            self.press_clear()
            return True
        if key == "Backspace":
            self.press_back()
            return True
        return False

    # ── 启动：把按钮接到行为 ──
    #
    # 事件绑定要的是一段无参数回调，而 press_digit 需要一个数字，
    # 所以每个键各给一个小方法：脚本模型里没有 lambda 与 partial，
    # 这比为了绑定去扩展语言便宜得多，生成出来也只是一次直接调用。

    def press_0(self):
        self.press_digit("0")

    def press_1(self):
        self.press_digit("1")

    def press_2(self):
        self.press_digit("2")

    def press_3(self):
        self.press_digit("3")

    def press_4(self):
        self.press_digit("4")

    def press_5(self):
        self.press_digit("5")

    def press_6(self):
        self.press_digit("6")

    def press_7(self):
        self.press_digit("7")

    def press_8(self):
        self.press_digit("8")

    def press_9(self):
        self.press_digit("9")

    def press_add(self):
        self.press_operator("+")

    def press_sub(self):
        self.press_operator("-")

    def press_mul(self):
        self.press_operator("×")

    def press_div(self):
        self.press_operator("÷")

    def onCreate(self):
        on_click(find("key0"), press_0)
        on_click(find("key1"), press_1)
        on_click(find("key2"), press_2)
        on_click(find("key3"), press_3)
        on_click(find("key4"), press_4)
        on_click(find("key5"), press_5)
        on_click(find("key6"), press_6)
        on_click(find("key7"), press_7)
        on_click(find("key8"), press_8)
        on_click(find("key9"), press_9)

        on_click(find("dot"), press_dot)
        on_click(find("add"), press_add)
        on_click(find("sub"), press_sub)
        on_click(find("mul"), press_mul)
        on_click(find("div"), press_div)
        on_click(find("equals"), press_equals)
        on_click(find("clear"), press_clear)
        on_click(find("back"), press_back)
        on_click(find("sign"), press_sign)
        on_click(find("percent"), press_percent)

        self.show()
        self.show_history("")
