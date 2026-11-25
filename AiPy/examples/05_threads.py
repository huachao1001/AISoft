# 示例 5：把工作分到后台线程，用锁保护共享状态。
#
# 这份脚本演示 `threading` 库，它和 `text` 一样是**独立静态库**
# （libs/threading），不参与构建的东西不会出现在产物里：脚本只用
# `threading.Thread` 和 `threading.Lock` 时，`Event`/`waitAll` 的代码
# 一个字节都不会被链进来。
#
#   python tools\aipyic\aipyic.py --exe examples\05_threads.py threads.exe ^
#       --build-dir build --lib-dir build
#
# ── 与标准库的写法对照 ──
#
# CPython：
#     t = threading.Thread(target=worker, args=(0, 25))
#     t.start(); t.join()
#
# AiPy 里函数不是值（`Value` 没有函数类型，`g = f` 编译不过），也不支持
# 关键字传参，所以 target 写**函数名**：
#     t = threading.Thread("worker", 0, 25)
#     t.start(); t.join()
#
# start/join/is_alive 的语义与标准库一致。
#
# ── 这个例子算的是什么 ──
#
# 四个后台任务各自累加一段区间，主线程等它们全部结束后校验总额。故意让
# 每个任务都要改同一个全局求和，以便演示 `with lock:`——它是唯一能保证
# 「两个线程不同时改同一个变量」的写法。
#
# ── 注意：不与 CPython 比对输出 ──
#
# `threading` 在 CPython 里确实存在，但 AiPy 的 `Thread` 第一个参数是**函数
# 名**而不是 target 函数，所以这份示例不与 CPython 逐行比对（见
# tests/check_examples.py），而是断言关键结果。

import threading

# 每个任务负责的区间长度。
CHUNK = 25

# 汇总结果。多个任务都会写它，所以每次写都要持锁。
TOTAL = 0
lock = threading.Lock()


def add_chunk(start, count):
    """把 [start, start+count) 累加进 TOTAL。

    `with lock:` 在退出时一定调 __exit__ —— 包括提前 return、循环里 break
    或 continue。锁没释放的症状是「程序偶发卡死」，而卡死点离这里很远。
    """
    subtotal = 0
    cursor = start
    while cursor < start + count:
        subtotal = subtotal + cursor
        cursor = cursor + 1

    # 函数里改模块级变量必须写 global，这是编译期强制检查的。
    global TOTAL
    with lock:
        TOTAL = TOTAL + subtotal

    return subtotal


# 起四个线程，覆盖 0..99 四个区间，然后逐个 join。
# 报出主线程现在的线程数：此刻后台还没开始跑，应当只有主线程。
print("启动前线程数: " + str(threading.active_count()))

threads = []
index = 0
while index < 4:
    t = threading.Thread("add_chunk", index * CHUNK, CHUNK)
    threads.append(t)
    t.start()
    index = index + 1

for t in threads:
    t.join()

print("完成的任务数: " + str(len(threads)))

print("汇总 TOTAL = " + str(TOTAL))

# ── 自检：算对了没有 ──
# 0+1+...+99 = 4950。打包成 exe 之后没人会读源码，所以关键结果自己验。
# 变量名不能与 add_chunk 里的局部量同名（模块级与函数局部同名会报错）。
expected = 0
n = 0
while n < CHUNK * 4:
    expected = expected + n
    n = n + 1

if TOTAL != expected:
    print("自检失败：TOTAL 应当是 " + str(expected))

# join 之后线程都不在了，但主线程还能继续用这把锁——上面任何一次 with
# 漏了释放，这里就会卡死。
with lock:
    print("主线程仍能拿到锁")

print("线程名: " + threading.current_thread())
