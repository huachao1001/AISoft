// `threading.Lock()` —— 互斥锁，支持 `with`。
//
// ── 为什么它是个 kObject 而不是 kHost ──
//
// kHost 只是「原样存、原样带回去」的不透明指针，脚本不能对它调方法。
// 锁必须有 acquire/release/__enter__/__exit__，所以需要方法表（见
// value.h「带方法的宿主对象」）。
//
// ── 为什么 __enter__ 返回 self ──
//
// `with lock as l:` 的 `l` 应当就是这把锁（CPython 的 Lock 也这么做）。
// 这里直接返回接收者 Value：它已经指向那份 HeapData，复制句柄只是把引用
// 计数 +1。**不能用 host 重新包一个 Value**——那会造出第二份 HeapData，
// 两份各自 delete 同一个 host，结果就是 double free。这个坑踩过。
#include "aipy/threading/lock.h"
#include "aipy/pylib.h"

#include <mutex>
#include <new>

namespace aipy {
namespace threading {
namespace {

struct LockState {
  std::mutex mutex;
};

void destroyLock(void* host) { delete static_cast<LockState*>(host); }

// 取锁状态。self 一定是 kObject（VM 只在这一支里转发），host 可能为空
// 只发生在分配失败时，那种情况返回 nullptr 让调用方给默认值。
LockState* stateOf(const Value& self) {
  const ObjectData* data = self.asObject();
  if (data == nullptr || data->host == nullptr) return nullptr;
  return static_cast<LockState*>(data->host);
}

Value acquireMethod(const Value& self, const Value* args, int count) {
  LockState* state = stateOf(self);
  if (state == nullptr) return Value(false);
  // acquire(False) 是「试一次」：AiPy 没有异常通道，拿不到锁时脚本没地方
  // 被唤醒，做不出阻塞超时。这与 CPython 的 acquire(blocking=False) 一致。
  if (count >= 1 && !args[0].toBool()) return Value(state->mutex.try_lock());
  state->mutex.lock();
  return Value(true);
}

Value releaseMethod(const Value& self, const Value*, int) {
  LockState* state = stateOf(self);
  if (state != nullptr) state->mutex.unlock();
  return Value();
}

Value enterMethod(const Value& self, const Value*, int) {
  LockState* state = stateOf(self);
  if (state != nullptr) state->mutex.lock();
  return self;
}

Value exitMethod(const Value& self, const Value*, int) {
  LockState* state = stateOf(self);
  if (state != nullptr) state->mutex.unlock();
  return Value();
}

const MethodEntry kLockMethods[] = {
    {"acquire", &acquireMethod},
    {"release", &releaseMethod},
    {"__enter__", &enterMethod},
    {"__exit__", &exitMethod},
    {nullptr, nullptr},
};

const MethodTable kLockTable = {kLockMethods};

Value lockImpl(const Value*, int) {
  // 不抛异常：AiPy 没有异常通道。分配失败返回 None，脚本会看到「lock 是
  // None」，比崩掉好排查。
  LockState* state = new (std::nothrow) LockState();
  if (state == nullptr) return Value();
  return Value::object(state, &kLockTable, &destroyLock);
}

}  // namespace

AIPY_PYLIB_FUNCTION(threading, Lock, lockImpl);

}  // namespace threading
}  // namespace aipy
