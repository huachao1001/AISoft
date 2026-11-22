# 笔记：Python + XML 的最小「真实软件」
#
# 演示的能力：
#   * 文件读写（read_file / write_file）
#   * 文件关联（.pynote 双击用本程序打开 → open_file）
#   * 系统托盘（右键菜单退出，左键唤回窗口）
#   * 关窗拦截（有未保存内容时先拦住）
#   * TextField 的 onChange 实时回调（字数统计、脏标记）
#
# 界面在 assets/layouts/notes.xml，这里只写行为。
#
# 窗口标题、尺寸、文件关联、托盘提示全部在同目录的 manifest.xml 里——
# 它们是**应用级别的声明**，不是行为，也不该因为业务逻辑变化而重新生成。
#
# 与 Android 一致：一个入口界面是一个继承 Activity 的类，onCreate 里做绑定。
# 状态写成 self.xxx：字段天然属于这个实例，方法里改它不用写 global。


class NotesActivity(Activity):
    # self.path 为 None 表示「还没保存过」
    path = None
    dirty = False
    # 程序自己往文本框里填内容时置真，避免刚打开文件就被标成「未保存」
    suppress = False

    def current_name(self):
        # 用函数而不是三元表达式：这里恰好也就两个分支，
        # 写成方法读起来更直白。
        if self.path is not None:
            return self.path
        return "新笔记"

    def refresh_status(self):
        if self.dirty:
            set_text(find("status"), "未保存 · " + self.current_name())
        else:
            set_text(find("status"), "已保存 · " + self.current_name())
        body = text(find("body"))
        set_text(find("count"), str(len(body)) + " 字")

    def mark_dirty(self):
        if self.suppress:
            return
        self.dirty = True
        self.refresh_status()

    def on_title_change(self, value):
        self.mark_dirty()

    def on_body_change(self, value):
        self.mark_dirty()

    def new_note(self):
        self.suppress = True
        set_text(find("title"), "")
        set_text(find("body"), "")
        self.suppress = False
        self.path = None
        self.dirty = False
        self.refresh_status()

    def save_to(self, target):
        # write_file 成功返回 None，失败返回错误描述
        header = text(find("title")).strip()
        body = text(find("body"))
        error = write_file(target, header + "\n" + body)
        if error is not None:
            set_text(find("status"), "保存失败：" + error)
            return False
        self.path = target
        self.dirty = False
        self.refresh_status()
        return True

    def default_path(self):
        home = home_directory()
        if home is None:
            return "pyui-note.pynote"
        return join_path(home, "pyui-note.pynote")

    def save(self):
        if self.path is None:
            return self.save_to(self.default_path())
        return self.save_to(self.path)

    def open_note(self, target):
        content = read_file(target)
        if content is None:
            set_text(find("status"), "打不开：" + target)
            return
        # 第一行是标题，其余是正文；空文件也要能打开
        lines = content.split("\n")
        self.suppress = True
        set_text(find("title"), lines[0])
        remainder = content[len(lines[0]):]
        if remainder.startswith("\n"):
            remainder = remainder[1:]
        set_text(find("body"), remainder)
        self.suppress = False
        self.path = target
        self.dirty = False
        self.refresh_status()

    def open_file(self, target):
        # 双击 .pynote 时由宿主调进来。run() 是在窗口建好之后才调它的，
        # 所以这里可以安全地 touch 控件。
        self.open_note(target)

    def on_close(self):
        # 返回 True 表示「别关」。有未保存内容时把窗口留在原地，
        # 让用户自己决定——弹确认框需要模态对话框，那是另一套东西。
        if not self.dirty:
            return False
        set_text(find("status"), "有未保存的修改，请先保存")
        return True

    def on_tray_activate(self):
        refresh()

    def tray_menu(self):
        return [menu_item("show", "显示窗口"),
                menu_item("save", "保存"),
                menu_separator(),
                menu_item("quit", "退出")]

    def on_tray_command(self, command):
        if command == "save":
            self.save()
            return
        if command == "quit":
            self.dirty = False  # 从托盘退出视为明确放弃修改
            quit(0)
            return
        refresh()

    def open_clicked(self):
        # 没有系统文件对话框时，打开「保存位置的那个文件」——
        # 让这个按钮在演示里也有确定的行为，而不是什么都不做。
        self.open_note(self.default_path())

    def onCreate(self):
        on_change(find("title"), on_title_change)
        on_change(find("body"), on_body_change)
        on_click(find("save"), save)
        on_click(find("new"), new_note)
        on_click(find("open"), open_clicked)
        focus(find("title"))
        self.refresh_status()
