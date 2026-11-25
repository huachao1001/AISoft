// 线程池、可等待结果、按名字提交：`threading` pylib 的共用底座。
//
// ── 为什么单独一个文件 ──
//
// 每个导出函数（Thread/start/poll/pending/waitAll）共用同一个池，但**它们
// 不能和池写在同一个编译单元**：MSVC 链接器按 .obj 抽取，四合一就意味着
// 「用了 start 就把 poll/pending/waitAll 全拉进来」，函数级按需就失效了。
//
// 所以池放这里，导出函数各自一个 .cpp、各自一行注册项。
//
// ── 为什么只用一个 mutex ──
//
// 「已完成结果」「未完成计数」是一份状态的两个侧面，必须一起改。分两把锁
// 时 waitAll 的谓词会读到撕裂的组合——第一版就踩了：谓词写成「结果空 且
// 未完成数为 0」，而结果只有 waitAll 自己会取走，于是它永远等不到，死锁。
#include "aipy/threading/pool.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace aipy {
namespace threading {
namespace {

// 当前线程的名字。worker 的名字是固定的 "Worker-N"，用静态字符串指针
// 而不是 thread_local std::string：后者每次线程启动都要构造析构一个
// std::string，而这个名字是常量。
thread_local const char* g_threadName = "MainThread";

class Pool {
 public:
  static Pool& instance() {
    // 函数内静态：C++11 保证多线程安全的一次性初始化。
    static Pool pool;
    return pool;
  }

  int workerCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(workers_.size());
  }

  // 正在执行的任务数。任务从被 worker 取走到结果完成之间算「在跑」。
  int runningCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
  }

  long long submit(std::function<Value()> task, std::shared_ptr<Future> future) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureStarted();
    const long long id = ++nextId_;
    tasks_.push_back(Job{id, std::move(task), std::move(future)});
    ++outstanding_;
    wake_.notify_one();
    return id;
  }

  Value poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (results_.empty()) return Value();
    Value value = std::move(results_.front());
    results_.pop_front();
    return value;
  }

  int pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(results_.size());
  }

  Value waitAll() {
    std::unique_lock<std::mutex> lock(mutex_);
    // 谓词只等「全部任务跑完」，不等结果被取走——取走是这里自己要做的事。
    // 带 future 的任务（Thread.join 用）不会把结果放进队列，join 自己取。
    done_.wait(lock, [this] { return outstanding_ == 0; });

    Value::List items;
    items.reserve(results_.size());
    while (!results_.empty()) {
      items.push_back(std::move(results_.front()));
      results_.pop_front();
    }
    return Value::list(std::move(items));
  }

 private:
  struct Job {
    long long id;
    std::function<Value()> task;
    std::shared_ptr<Future> future;  // 为空表示结果进队列，否则交给 join
  };

  Pool() = default;

  ~Pool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
  }

  void ensureStarted() {
    if (!workers_.empty()) return;
    // 4 个：脚本的后台任务通常是短任务，太多线程只增加调度开销。
    for (int i = 0; i < 4; ++i) {
      workers_.emplace_back([this, i] {
        // 每个 worker 一个固定名字，给 current_thread() 用。
        // 名字表放在函数内静态：线程启动时登记一次，字符串的生命周期
        // 与进程相同，所以 thread_local 存指针是安全的。
        static const char* const names[] = {"Worker-0", "Worker-1",
                                           "Worker-2", "Worker-3"};
        g_threadName = names[i];
        workerLoop();
      });
    }
  }

  void workerLoop() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (stopping_ && tasks_.empty()) return;
        job = std::move(tasks_.front());
        tasks_.pop_front();
        ++running_;
      }

      // 任务在锁外跑：跑得慢不该卡住别的提交。
      Value value = job.task();

      // future 在锁外完成：它的 wait 可能正在别的线程上等着。
      if (job.future) job.future->complete(std::move(value));

      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!job.future) results_.push_back(std::move(value));
        --outstanding_;
        --running_;
      }
      done_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable done_;
  std::deque<Job> tasks_;
  std::deque<Value> results_;
  std::vector<std::thread> workers_;
  long long nextId_ = 0;
  int outstanding_ = 0;
  int running_ = 0;
  bool stopping_ = false;
};

}  // namespace

long long submit(std::function<Value()> task) {
  return Pool::instance().submit(std::move(task), nullptr);
}

long long submitNamed(const std::string& name, const std::vector<Value>& args,
                      std::shared_ptr<Future> future) {
  // 当前程序由 VM 通过 thread_local 提供：扩展函数只拿到参数，拿不到
  // Program，而子线程要知道跑哪个程序的哪个函数。
  const vm::Program* program = vm::currentProgram();
  if (program == nullptr) return 0;

  // 指针必须活过子线程：launcher 的 Program 通常活到进程结束，但这里不
  // 假设它。Program 很大（整个字节码），拷贝一份进任务是浪费；交给调用方
  // 保证生命周期——脚本层面它一定活到 main 结束。
  const vm::Program* shared = program;

  bool found = false;
  for (const vm::Function& candidate : shared->functions) {
    if (candidate.name != name) continue;
    found = true;
    if (static_cast<int>(args.size()) != candidate.paramCount) return 0;
    break;
  }
  if (!found) return 0;

  return Pool::instance().submit(
      [shared, name, args]() -> Value {
        // 子线程自己一个 Machine：Machine 持有帧栈，共享会让两个执行循环
        // 同时改同一份状态。全局槽是进程级共享的（见 Machine::globals_），
        // 所以 `global X` 写的是主线程看得见的那个槽。
        vm::Machine machine;
        const vm::Result result = machine.callFunction(*shared, name, args);
        if (result.failed) return Value();
        return result.value;
      },
      std::move(future));
}

Value poll() { return Pool::instance().poll(); }
int pending() { return Pool::instance().pending(); }
Value waitAll() { return Pool::instance().waitAll(); }
int runningCount() { return Pool::instance().runningCount(); }

std::string currentThreadName() { return std::string(g_threadName); }

}  // namespace threading
}  // namespace aipy
