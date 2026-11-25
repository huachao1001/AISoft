// `threading` pylib 的对外契约说明（无代码，仅供阅读）。
//
// 为什么把说明和实现分开：实现散在 fn_*.cpp 里，一个读者想知道「这个库
// 到底提供什么、与 CPython 差在哪」，不该去翻十来个文件。
//
// ── 它是「一切皆是第三方库」的第一个样本 ──
//
// 线程不是语言语义，是具体能力。所以它不在 `src/` 里、不进 `aipy.lib`，
// 而是独立静态库 `libs/threading`，按脚本**实际调用到的函数**逐个挑代码
// 链接：脚本只用 `threading.pending`，`Thread` 的 obj 就不会进产物。
//
// ── 接口与标准库对齐到哪一步 ──
//
//   threading.Thread("worker", 参数...)   t.start() / t.join() / t.is_alive() / t.name()
//   threading.Lock()                      acquire / release / with
//   threading.Event()                     set / clear / is_set / wait
//   threading.current_thread()            线程名（MainThread / Worker-N）
//   threading.active_count()
//
// 差别只有一处：**target 是名字，不是函数**。AiPy 里函数不是值（`Value`
// 没有函数类型，`g = f` 编译不过），也不支持关键字传参，所以写不出
// `Thread(target=worker)`。这是语言事实，不是 API 取舍。
//
// 另有三个 AiPy 扩展（标准库没有）：`start(name, ...)` 一行提交、
// `waitAll()` 收全部结果、`poll()`/`pending()` 非阻塞取——它们服务于
// 「丢出去、收结果」这种最常见的脚本写法。
//
// ── 线程之间怎么共享数据 ──
//
// 共享内存，不拷贝：`Value` 的引用计数已经是原子的（见 value.h 文件头），
// 所以两个线程各持同一个 list 的句柄是安全的。脚本里「子线程往 list 里
// 写、主线程读」是合法用法。
//
// 但要注意：**共享可变容器本身仍需要同步**。引用计数只保证「不会因为
// 计数竞争而提前释放或泄漏」，不保证「两个线程同时 append 一个 list 是
// 安全的」——那是数据竞争，与引用计数是两件事，用 `threading.Lock()`。
//
// ── 为什么子线程必须有独立的 Machine ──
//
// `Machine` 持有帧栈。两个线程共用一个 Machine 等于让两个执行循环同时改
// 同一份帧栈。所以每个子线程自己 new 一个 Machine；全局槽是进程级共享的
// （见 Machine::globals_），`global X` 写的仍是主线程看得见的那个槽。
//
// 共享状态与提交入口的声明在 pool.h；这里只讲边界。
#pragma once
