# 示例 9：把扩展名关联到本程序，双击文件就打开它。
#
# 演示 `os.fileassoc` 库。打包成 exe 后：
#
#   dist\pcc.exe --exe examples\09_fileassoc.py mydoc.exe ^
#       --build-dir build --lib-dir build
#
#   mydoc.exe --install    # 写入关联
#   （之后双击任意 .aipydoc 文件都会启动 mydoc.exe）
#   mydoc.exe --uninstall  # 撤销关联
#
# 不带参数直接双击 .aipydoc 文件时，系统会把文件路径作为第一个参数传进来，
# 脚本靠 `sys.argv()` 取到它。
#
# ── 为什么改注册表不用管理员权限 ──
#
# 只写 `HKCU\Software\Classes`（当前用户），不碰 HKLM。所以不弹 UAC，也不会
# 影响这台机器上的其他用户。
#
# ── 一个 Windows 的限制 ──
#
# Win8 之后，用户已经在「打开方式」里选过的扩展名会带一条带哈希校验的
# UserChoice，程序无法合法改写它（微软刻意防止程序互相抢关联）。所以：
#
#   全新的扩展名  → 关联直接生效
#   已被占用的    → 系统弹「你要用什么打开」，用户选一次本程序即可
#
# 硬改 UserChoice 的后果是关联显示成「未知应用」，比不做更糟，所以这里不做。

import os.fileassoc
import sys

# 本程序声明的扩展名。取一个不会与真实文件类型撞车的名字。
EXTENSION = ".aipydoc"


def show_usage():
    print("用法:")
    print("  mydoc.exe --install     写入关联")
    print("  mydoc.exe --uninstall   撤销关联")
    print("  mydoc.exe 某文件" + EXTENSION + "     打开文件")
    print("  mydoc.exe --status      查看关联状态")


def install():
    """写入关联。返回空串表示成功。"""
    error = os.fileassoc.register(EXTENSION, "AiPy.Doc")
    if error != "":
        print("写入关联失败: " + error)
        return

    # 用本 exe 自带的图标。想用单独的 .ico 就把路径传进来。
    icon_error = os.fileassoc.icon(EXTENSION, "")
    if icon_error != "":
        print("设置图标失败: " + icon_error)
        return

    print("已关联 " + EXTENSION + " → 本程序")
    print("现在双击任意 " + EXTENSION + " 文件都会用本程序打开。")


def uninstall():
    """撤销关联。只删自己写的键，不会拆掉别的程序的关联。"""
    error = os.fileassoc.unregister(EXTENSION)
    if error != "":
        print("撤销关联失败: " + error)
        return
    print("已撤销 " + EXTENSION + " 的关联")


def status():
    """看当前状态。不写注册表，只读。"""
    prog_id = os.fileassoc.query(EXTENSION)
    if prog_id == "":
        print(EXTENSION + " 当前没有关联")
        return
    owned = os.fileassoc.owned(EXTENSION)
    print(EXTENSION + " → " + prog_id)
    print("是本程序吗: " + str(owned))


def open_file(path):
    """被双击时走这里。真程序会在这里解析文件内容。"""
    print("打开文件: " + path)
    try:
        with open(path) as handle:
            text = handle.read()
        print("共 " + str(len(text)) + " 个字符")
        print("--- 内容 ---")
        print(text)
    except:
        print("读不到这个文件")


def main():
    # argv[0] 是 exe 自己，之后是用户传的。
    args = sys.argv()
    if len(args) < 2:
        show_usage()
        return

    command = args[1]
    if command == "--install":
        install()
    elif command == "--uninstall":
        uninstall()
    elif command == "--status":
        status()
    else:
        open_file(command)


main()
