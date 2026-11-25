// `threading` pylib 的内部共用接口：池本体 + 可等待结果。
//
// 为什么单独一个头：导出函数各在一个 .cpp 里，它们都要调池，但
// **不能**各自 include 一份池的实现——那会让链接器抽出重复代码。
#pragma once

#include "aipy/vm.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aipy {
namespace threading {

// 一个任务的结果，可以被单独等待。
//
// 与池里的结果队列分开：队列是 `waitAll()` 那种「收全部结果」的语义，
// 而 `Thread.join()` 只等**自己那一个**。混在同一个队列里会让 join 取走
// 别人的结果。
class Future {
 public:
  void complete(Value value) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      value_ = std::move(value);
      ready_ = true;
    }
    done_.notify_all();
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this] { return ready_; });
  }

  bool ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
  }

  // 取走结果。只应调用一次；重复调用拿到的是空值。
  Value take() {
    std::lock_guard<std::mutex> lock(mutex_);
    Value value = std::move(value_);
    value_ = Value();
    return value;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable done_;
  bool ready_ = false;
  Value value_;
};

// 提交一个返回 Value 的任务，返回任务号。
long long submit(std::function<Value()> task);

// 按脚本函数名提交到池里，结果写进 `future`（可为 nullptr，表示走旧的结果
// 队列）。函数不存在或参数个数不对返回 0，什么都不提交。
//
// 校验发生在**提交时**：等跑到子线程才发现「没这个函数」，错误就与脚本里
// 那一行脱钩了。
long long submitNamed(const std::string& name, const std::vector<Value>& args,
                      std::shared_ptr<Future> future);

Value poll();
int pending();
Value waitAll();

// 池里正在执行任务的线程数（空闲为 0）。
//
// 为什么不是「池里创建了几个工作线程」：池的工作线程是复用的，join 之后
// 仍然活着，那样 active_count() 会一直返回 5，与 CPython「join 完回到 1」
// 的语义相悖。数正在跑的任务才对得上脚本的直觉。
int runningCount();

// 当前线程的名字："MainThread" 或 "Worker-N"。给 current_thread 用。
std::string currentThreadName();

}  // namespace threading
}  // namespace aipy
