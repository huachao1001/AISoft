// `threading.Lock()` 的公开声明。
//
// 锁是一个 kObject（带方法表），不是 kHost——脚本要能对它调
// acquire/release/__enter__/__exit__（见 value.h「带方法的宿主对象」）。
#pragma once

namespace aipy {
namespace threading {

// 实现见 fn_lock.cpp。这里只说明契约：
//
//   lock = threading.Lock()
//   lock.acquire()          // 阻塞加锁
//   lock.release()
//   with lock:              // __enter__ 加锁、__exit__ 解锁
//       ...
//   with lock as l:         // l 就是这把锁
//       ...
//
// 为什么没有 Lock(blocking=False)：AiPy 没有异常通道，拿不到锁时脚本
// 无法被唤醒，所以非阻塞语义只能做成「试一次」。acquire(False) 是这个
// 意思，而不是 CPython 的 timeout 参数。

}  // namespace threading
}  // namespace aipy
