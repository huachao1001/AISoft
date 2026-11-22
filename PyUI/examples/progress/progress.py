# 进度演示：动画、拖放、原生右键菜单
#
# 演示的能力：
#   * on_frame 帧定时器（manifest 里的 frameInterval 驱动，脚本自己推进状态）
#   * ProgressBar 的确定 / 不确定两种形态
#   * 窗口级拖放（on_file_drop），拖进来的路径用脚本格式化后显示
#   * 系统原生右键菜单（popup_menu + menu_item）
#
# 界面在 assets/layouts/progress.xml。
#
# 窗口尺寸与帧定时器间隔在 manifest.xml 里——那是应用级别的声明，
# 不是行为。
#
# 与 Android 一致：一个入口界面是一个继承 Activity 的类，onCreate 里做绑定。
# 状态写成 self.xxx 而不是模块级变量 + global：字段天然属于这个实例，
# 方法里改它不需要声明任何东西。


class ProgressActivity(Activity):
    running = False
    level = 0.0
    # 每帧推进 2% → 大约 0.8 秒走完，肉眼能看清又不至于太慢。
    step = 2.0
    dropped = []

    def show_status(self, message):
        set_text(find("status"), message)

    def show_progress(self):
        set_progress(find("known"), self.level)
        set_label(find("known"), str(round(self.level * 100.0)) + "%")

    def start(self):
        self.running = True
        self.show_status("处理中…")

    def reset(self):
        self.running = False
        self.level = 0.0
        self.dropped = []
        self.show_progress()
        set_text(find("drop_text"), "拖放文件到这里")
        self.show_status("就绪")

    def on_frame(self):
        # 只有真正在跑时才推进。帧定时器一直在响（宿主只要有活动就会调
        # on_frame），无条件累加会让进度条自己往前走。
        if not self.running:
            return
        self.level = self.level + self.step / 100.0
        if self.level >= 1.0:
            self.level = 1.0
            self.running = False
            self.show_status("完成")
        self.show_progress()

    def on_file_drop(self, paths):
        # paths 是字符串列表；全部记下来，只展示最后一个的名字。
        index = 0
        count = len(paths)
        while index < count:
            self.dropped.append(paths[index])
            index = index + 1
        if count > 0:
            set_text(find("drop_text"), "收到 " + str(count) + " 个文件")
            self.show_status("最后一个：" + paths[count - 1])
            # 收到文件就自动跑一遍，让「拖放 → 进度」这条链路在演示里成立
            self.level = 0.0
            self.start()

    # 注意不能把这个方法叫 on_context_menu：那是脚本层的绑定函数名，
    # 同名会被当成「给控件挂右键回调」而不是「这里定义的动作」。
    def show_menu(self):
        # 系统原生菜单：位置由平台决定（鼠标当前处），返回选中项 id，
        # 取消时是空串。
        choice = popup_menu([menu_item("start", "开始处理"),
                             menu_item("reset", "重置"),
                             menu_separator(),
                             menu_item("quit", "退出")])
        if choice == "start":
            self.start()
        elif choice == "reset":
            self.reset()
        elif choice == "quit":
            self.running = False
            quit(0)

    def onCreate(self):
        on_click(find("start"), start)
        on_click(find("reset"), reset)
        on_context_menu(find("drop"), show_menu)
        self.show_progress()
        self.show_status("就绪")
