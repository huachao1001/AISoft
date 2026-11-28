# 文件查看器：核心不含 IO，读写全部来自 fs 扩展
#
# 这个示例的作用不是演示功能，而是**验证边界**：
#   * 脚本里 `import fs` 能让 pyuic 找到清单、生成 include 与调用；
#   * pyui_extc 据此把 fs.pylib 放进链接集；
#   * 如果把这个 import 删掉，产物里就不该再有 fs 的任何代码。
#
# 界面在 assets/layouts/fileviewer.xml。
import fs


class FileViewerActivity(Activity):
    path = ""
    content = ""

    def status(self, message):
        set_text(find("out"), message)

    def load(self):
        if self.path == "":
            self.status("还没有文件路径：先点「写入」生成一个")
            return
        text = fs.read_file(self.path)
        if is_none(text):
            self.status("读不到 " + self.path)
            return
        self.content = text
        self.status(self.content)

    def save(self):
        # 写到用户主目录，避免为了一个演示去实现完整的保存对话框
        home = fs.home_directory()
        if is_none(home):
            self.status("拿不到用户主目录")
            return
        self.path = fs.join_path(home, "pyui_fileviewer_demo.txt")
        error = fs.write_file(self.path, "由 PyUI 文件查看器写入\n第二行")
        if is_none(error):
            self.status("已写入 " + self.path)
        else:
            self.status("写入失败：" + error)

    def onCreate(self):
        on_click(find("load"), load)
        on_click(find("save"), save)
        self.status("就绪：点「写入」创建一个示例文件")
