# 示例 8：托盘图标 + 右键菜单 + 左键回调。
#
# 演示 `os.tray` 库。打包成 exe 后运行，通知区域会出现一个图标：
#
#   dist\pcc.exe --exe examples\08_tray.py tray.exe ^
#       --build-dir build --lib-dir build
#
# 注意产物是**图形子系统**：脚本用了 `os.tray`，打包器就把控制台窗口去掉，
# 双击时不会先闪一个黑框。这也是这个示例不能用 `python 08_tray.py` 跑的原因
# ——CPython 没有 `os.tray`。
#
# ── 托盘程序的形状 ──
#
# 与普通脚本的区别只有一处：最后一句是**阻塞**的 `os.tray.run()`。它把主
# 线程交给 Windows 的消息循环，之后的一切都由回调驱动。
#
# 回调在 run() 的线程上同步执行，所以回调里读写模块级变量不需要加锁。

import os.tray

# 点击次数。左键与「点我一下」菜单项都写它，用来演示回调能看到同一份状态。
COUNT = 0


def bump():
    """左键与菜单项共用的回调。"""
    global COUNT
    COUNT = COUNT + 1
    # 气泡提示只能在图标挂上之后发，所以只能从回调里调。
    os.tray.notify("你好", "你点了 " + str(COUNT) + " 次")


def quit_app():
    """退出消息循环。run() 返回后脚本继续往后走。"""
    os.tray.quit()


# 图标：空路径 = 系统默认图标。第一个参数也可以是 .ico 文件的路径。
os.tray.icon("", "AiPy 托盘示例")

# 菜单项按加入顺序显示。第一个参数是 id，回调按同一个 id 查找。
os.tray.item("bump", "点我一下")
os.tray.separator()
os.tray.item("quit", "退出")

# 绑回调。第二个参数是**函数值**（不是名字），所以 lambda 也能用。
os.tray.on("bump", bump)
os.tray.on("quit", quit_app)
# 保留 id "click" 表示左键。
os.tray.on("click", bump)

print("托盘图标已挂上，右键看菜单，左键点一下。")

# 阻塞在这里直到 quit_app（或点菜单里的「退出」）。
os.tray.run()

print("已退出，共点了 " + str(COUNT) + " 次。")
