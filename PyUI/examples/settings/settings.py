# 设置面板：把八类控件排在一屏，顺便做成一个真能记住选择的程序
#
# 演示的能力：
#   * 所有控件类型一起出现（TextView / Button / CheckBox / RadioButton /
#     TextField / ProgressBar / Slider / 容器三类）
#   * 运行期换主题（set_theme）——这是唯一一个会改到「整窗配色」的动作
#   * 配置持久化（fs 扩展：fs.read_file / fs.write_file 写到主目录）
#   * 派生状态：配置完整度由各控件当前值算出来，任何改动都立刻反映到进度条
#
# 界面在 assets/layouts/settings.xml。两者的接口只有 id。
#
# 窗口标题与尺寸在 manifest.xml 里。
#
# 与 Android 一致：状态是 Activity 的字段（self.xxx）。同一个对象的字段，
# 所以「读配置 → 改配置 → 保存」天然是一条链，不需要 global。
#
# 文件读写走 fs 扩展——核心只提供基础 Python 语义，碰系统资源的能力都在扩展里。
import fs


class SettingsActivity(Activity):
    # 默认值。配置文件不存在、或者某一项解析失败时，用的就是这些。
    theme = "light"
    nickname = ""
    volume = 60.0
    autostart = False
    compact = True
    # 写配置文件时置真：初始载入过程中的控件回调不应该把状态又标成「已改」
    loading = False

    def settings_path(self):
        home = fs.home_directory()
        if home is None:
            return "pyui-settings.txt"
        return fs.join_path(home, "pyui-settings.txt")

    # ── 状态 → 界面 ──

    def show_status(self, message):
        set_text(find("status"), message)

    def show_volume(self):
        # 滑块给的是 0..100 的浮点；显示时取整，避免「60.000001%」
        set_text(find("volume_value"), str(round(self.volume)) + "%")

    def completeness(self):
        # 四项都满足了才算「完整」，进度条直接表达这个比例。
        # 用加法而不是布尔求和：脚本里没有隐式 int(True)。
        score = 0
        if self.nickname != "":
            score = score + 1
        if self.volume > 0.0:
            score = score + 1
        if self.autostart:
            score = score + 1
        if self.compact:
            score = score + 1
        return score

    def show_completeness(self):
        ratio = self.completeness() / 4.0
        set_progress(find("ready"), ratio)
        set_text(find("ready_value"), str(round(ratio * 100.0)) + "%")

    def apply_theme(self):
        set_theme(self.theme)
        # 换肤不会自己重算「哪一项被选中」的那种视觉状态，
        # 但单选按钮的选中状态本来就是控件数据，这里只要同步一次。
        if self.theme == "dark":
            set_checked(find("theme_dark"), True)
            set_checked(find("theme_light"), False)
        else:
            set_checked(find("theme_light"), True)
            set_checked(find("theme_dark"), False)

    def refresh_all(self):
        self.apply_theme()
        self.show_volume()
        self.show_completeness()

    # ── 载入与保存 ──

    def parse_line(self, line):
        # 每行形如 "key=value"；空行与没有等号的行直接忽略。
        if line == "":
            return 0
        position = line.find("=")
        if position < 0:
            return 0
        key = line[0:position].strip()
        value = line[position + 1:].strip()
        if key == "theme":
            if value == "dark":
                self.theme = "dark"
            else:
                self.theme = "light"
        elif key == "nickname":
            self.nickname = value
        elif key == "volume":
            parsed = parse_number(value)
            if parsed is not None:
                self.volume = parsed
        elif key == "autostart":
            self.autostart = value == "true"
        elif key == "compact":
            self.compact = value == "true"
        return 1

    def load_settings(self):
        content = fs.read_file(self.settings_path())
        if content is None:
            # 第一次运行：配置文件还不存在，这不是错误。
            return
        # 用 for 遍历行：脚本里的 range 只支持整数区间，按行读更直接。
        for line in content.split("\n"):
            self.parse_line(line)

    def save_settings(self):
        # 手写拼接而不是 str.format：格式固定，写出来就是文件格式的唯一定义。
        body = ("theme=" + self.theme + "\n" +
                "nickname=" + self.nickname + "\n" +
                "volume=" + str(round(self.volume)) + "\n" +
                "autostart=" + self.bool_text(self.autostart) + "\n" +
                "compact=" + self.bool_text(self.compact) + "\n")
        error = fs.write_file(self.settings_path(), body)
        if error is not None:
            self.show_status("保存失败：" + error)
            return
        self.show_status("已保存到 " + self.settings_path())

    def bool_text(self, value):
        # 配置文件里写 true/false，与 Python 的 True/False 分开：
        # 读回来时大小写敏感，统一小写最省事。
        if value:
            return "true"
        return "false"

    # ── 控件回调 ──

    def on_nickname_change(self, value):
        self.nickname = value
        if self.loading:
            return
        self.show_completeness()
        self.show_status("昵称已改为 " + self.nickname)

    def on_volume_change(self, value):
        self.volume = value
        if self.loading:
            return
        self.show_volume()
        self.show_completeness()
        self.show_status("音量 " + str(round(self.volume)) + "%")

    def on_autostart_change(self, value):
        self.autostart = value
        if self.loading:
            return
        self.show_completeness()

    def on_compact_change(self, value):
        self.compact = value
        if self.loading:
            return
        self.show_completeness()

    def use_light(self):
        self.theme = "light"
        self.apply_theme()
        self.show_status("已切换到浅色主题")

    def use_dark(self):
        self.theme = "dark"
        self.apply_theme()
        self.show_status("已切换到深色主题")

    def reset(self):
        # 重置只改数据，然后再走一遍 refresh_all：控件状态全部由变量派生，
        # 不给「重置」单写一套 set_xxx，避免两条路径慢慢走偏。
        self.theme = "light"
        self.nickname = ""
        self.volume = 60.0
        self.autostart = False
        self.compact = True
        self.loading = True
        set_text(find("nickname"), self.nickname)
        set_value(find("volume"), self.volume)
        set_checked(find("autostart"), self.autostart)
        set_checked(find("compact"), self.compact)
        self.loading = False
        self.refresh_all()
        self.show_status("已恢复默认值")

    def onCreate(self):
        # 顺序：先载入（只改数据），再把数据推到控件上（loading 期间不回调），
        # 最后挂回调。反过来的话，初始化阶段的控件改动会被当成用户操作。
        self.load_settings()

        self.loading = True
        set_text(find("nickname"), self.nickname)
        set_value(find("volume"), self.volume)
        set_checked(find("autostart"), self.autostart)
        set_checked(find("compact"), self.compact)
        self.loading = False

        self.refresh_all()

        on_change(find("nickname"), on_nickname_change)
        on_change(find("volume"), on_volume_change)
        on_change(find("autostart"), on_autostart_change)
        on_change(find("compact"), on_compact_change)
        on_click(find("theme_light"), use_light)
        on_click(find("theme_dark"), use_dark)
        on_click(find("reset"), reset)
        on_click(find("save"), save_settings)
        self.show_status("就绪")
