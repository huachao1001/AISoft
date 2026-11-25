// `threading.Event()` —— 与标准库同名同语义的「一次性通知」。
//
// set()/clear()/is_set()/wait() 四个方法与 CPython 一致。wait() 阻塞到
// set() 被调用；因为 AiPy 没有异常通道，`wait(timeout)` 这种「超时返回
// False」的形态做不出来（超时是失败路径，得用返回值表达，而返回值已经被
// 「等到了」占用）。所以只支持无参 wait()。
#include "aipy/pylib.h"

#include <condition_variable>
#include <mutex>
#include <new>

namespace aipy {
namespace threading {
namespace {

struct EventState {
  std::mutex mutex;
  std::condition_variable changed;
  bool flag = false;
};

void destroyEvent(void* host) { delete static_cast<EventState*>(host); }

EventState* stateOf(const Value& self) {
  const ObjectData* data = self.asObject();
  if (data == nullptr || data->host == nullptr) return nullptr;
  return static_cast<EventState*>(data->host);
}

Value setMethod(const Value& self, const Value*, int) {
  EventState* state = stateOf(self);
  if (state == nullptr) return Value();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->flag = true;
  }
  // 先改状态再通知：否则被唤醒的线程可能读到旧的 flag 又睡回去。
  state->changed.notify_all();
  return Value();
}

Value clearMethod(const Value& self, const Value*, int) {
  EventState* state = stateOf(self);
  if (state == nullptr) return Value();
  std::lock_guard<std::mutex> lock(state->mutex);
  state->flag = false;
  return Value();
}

Value isSetMethod(const Value& self, const Value*, int) {
  EventState* state = stateOf(self);
  if (state == nullptr) return Value(false);
  std::lock_guard<std::mutex> lock(state->mutex);
  return Value(state->flag);
}

Value waitMethod(const Value& self, const Value*, int) {
  EventState* state = stateOf(self);
  if (state == nullptr) return Value();
  std::unique_lock<std::mutex> lock(state->mutex);
  state->changed.wait(lock, [state] { return state->flag; });
  return Value();
}

const MethodEntry kEventMethods[] = {
    {"set", &setMethod},
    {"clear", &clearMethod},
    {"is_set", &isSetMethod},
    {"wait", &waitMethod},
    {nullptr, nullptr},
};

const MethodTable kEventTable = {kEventMethods};

Value eventImpl(const Value*, int) {
  EventState* state = new (std::nothrow) EventState();
  if (state == nullptr) return Value();
  return Value::object(state, &kEventTable, &destroyEvent);
}

}  // namespace

AIPY_PYLIB_FUNCTION(threading, Event, eventImpl);

}  // namespace threading
}  // namespace aipy
