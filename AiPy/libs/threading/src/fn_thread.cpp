// `threading.Thread(name, *args)` —— 与标准库同名同形的线程对象。
//
// ── 与 CPython 的差别只有一处：target 是一个**名字** ──
//
// CPython 写 `Thread(target=worker)`，把函数当值传。AiPy 里函数不是值
// （`Value` 没有函数类型，`g = f` 编译不过），且不支持关键字传参，所以
// 只能是 `threading.Thread("worker", 参数...)`。start/join/is_alive 的
// 语义与标准库一致。
//
// ── join() 为什么不返回线程函数的返回值 ──
//
// CPython 的 join() 返回 None，工作结果通过共享对象或全局变量传出。
// 这里保持一致：join() 也只等完成。要收集返回值用 `threading.waitAll()`
// （AiPy 扩展，标准库没有对应物）。
#include "aipy/threading/pool.h"
#include "aipy/pylib.h"

#include <atomic>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace aipy {
namespace threading {
namespace {

struct ThreadState {
  std::string name;
  std::vector<Value> args;
  std::shared_ptr<Future> future;
  std::atomic<bool> started{false};
  long long id = 0;
  // 提交失败（函数不存在、参数个数不对）留个记号：start() 返回 False，
  // is_alive() 保持 False，join() 立刻返回。不抛异常——AiPy 没有异常通道。
  bool failed = false;
};

void destroyThread(void* host) { delete static_cast<ThreadState*>(host); }

ThreadState* stateOf(const Value& self) {
  const ObjectData* data = self.asObject();
  if (data == nullptr || data->host == nullptr) return nullptr;
  return static_cast<ThreadState*>(data->host);
}

Value startMethod(const Value& self, const Value*, int) {
  ThreadState* state = stateOf(self);
  if (state == nullptr) return Value(false);
  if (state->started.exchange(true)) return Value(false);  // 重复 start 无效

  state->future = std::make_shared<Future>();
  state->id = submitNamed(state->name, state->args, state->future);
  if (state->id == 0) {
    state->failed = true;
    return Value(false);
  }
  return Value();
}

Value joinMethod(const Value& self, const Value*, int) {
  ThreadState* state = stateOf(self);
  // 未 start 就 join：CPython 直接返回，不阻塞。
  if (state == nullptr || !state->started.load() || state->failed) return Value();
  if (state->future) state->future->wait();
  return Value();
}

Value isAliveMethod(const Value& self, const Value*, int) {
  ThreadState* state = stateOf(self);
  if (state == nullptr || !state->started.load() || state->failed) return Value(false);
  return Value(state->future && !state->future->ready());
}

Value nameMethod(const Value& self, const Value*, int) {
  ThreadState* state = stateOf(self);
  if (state == nullptr) return Value(std::string());
  return Value(state->name);
}

const MethodEntry kThreadMethods[] = {
    {"start", &startMethod},
    {"join", &joinMethod},
    {"is_alive", &isAliveMethod},
    {"name", &nameMethod},
    {nullptr, nullptr},
};

const MethodTable kThreadTable = {kThreadMethods};

Value threadImpl(const Value* args, int count) {
  if (count < 1) return Value();
  ThreadState* state = new (std::nothrow) ThreadState();
  if (state == nullptr) return Value();
  state->name = args[0].toString();
  for (int i = 1; i < count; ++i) state->args.push_back(args[i]);
  return Value::object(state, &kThreadTable, &destroyThread);
}

}  // namespace

AIPY_PYLIB_FUNCTION(threading, Thread, threadImpl);

}  // namespace threading
}  // namespace aipy
