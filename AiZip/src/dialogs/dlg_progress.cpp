// 进度对话框
//
// 工作线程只更新一份加锁的快照并请求刷新；控件只在 UI 线程上碰。
// 取消用原子标志，工作线程的进度回调读它返回 false 即可中断。
#include "dlg_progress.h"

#include "util.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/form_controls.h"
#include "AiUI/widgets/text_view.h"
#include "dialog_activity.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace ui {
namespace {

class DlgProgress : public DialogActivity {
 public:
  DlgProgress(std::wstring title, WorkFn work)
      : title_(std::move(title)), work_(std::move(work)) {}
  ~DlgProgress() override;

  bool succeeded() const { return succeeded_; }

 protected:
  const wchar_t* layoutName() const override { return L"dlg_progress"; }
  void onCreate() override;
  void onDestroy() override;
  bool onCloseRequest() override;
  bool onAccept() override { return true; }

 private:
  // 工作线程：跑 work，结束后请求 UI 线程收尾
  void startWork();
  // UI 线程：按快照刷新控件
  void applySnapshot();
  // UI 线程：工作结束，关闭窗口
  void finishWork();
  bool progressCallback(uint64_t completed, uint64_t total, const std::wstring& current);

  std::wstring title_;
  WorkFn work_;

  AiUI::TextView* fileLabel_ = nullptr;
  AiUI::TextView* detailLabel_ = nullptr;
  AiUI::ProgressBar* bar_ = nullptr;
  AiUI::Button* cancelButton_ = nullptr;

  std::atomic<bool> cancel_{false};
  std::atomic<bool> finished_{false};
  // 投递的回调可能在对话框销毁后才被消息循环取出；共享标志用来判断是否还该碰控件
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  bool succeeded_ = false;

  std::mutex snapshotMutex_;
  std::wstring currentFile_;
  uint64_t completed_ = 0;
  uint64_t total_ = 0;
  int64_t lastPostMillis_ = 0;

  std::thread worker_;
};

DlgProgress::~DlgProgress() {
  // 正常路径已在 onDestroy 里 join；这里是异常退出的兜底
  if (worker_.joinable()) {
    cancel_.store(true);
    worker_.join();
  }
}

bool DlgProgress::progressCallback(uint64_t completed, uint64_t total,
                                   const std::wstring& current) {
  if (cancel_.load(std::memory_order_relaxed)) return false;

  bool shouldPost = false;
  {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    completed_ = completed;
    total_ = total;
    if (!current.empty()) currentFile_ = current;

    // 限流 60ms：进度回调可能极密集，每次都投递会把消息队列灌满
    const int64_t now = AiUI::platform()->monotonicMillis();
    if (now - lastPostMillis_ >= 60) {
      lastPostMillis_ = now;
      shouldPost = true;
    }
  }
  if (shouldPost) {
    auto alive = alive_;
    AiUI::platform()->postToUiThread([this, alive] {
      if (alive->load()) applySnapshot();
    });
  }
  return !cancel_.load(std::memory_order_relaxed);
}

void DlgProgress::startWork() {
  worker_ = std::thread([this] {
    bool ok = false;
    try {
      ok = work_([this](uint64_t completed, uint64_t total, const std::wstring& current) {
        return progressCallback(completed, total, current);
      });
    } catch (...) {
      ok = false;
    }
    succeeded_ = ok;
    // 收尾必须回到 UI 线程：控件只能在那里销毁
    auto alive = alive_;
    AiUI::platform()->postToUiThread([this, alive] {
      if (alive->load()) finishWork();
    });
  });
}

void DlgProgress::applySnapshot() {
  if (!bar_) return;

  std::wstring file;
  uint64_t completed = 0;
  uint64_t total = 0;
  {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    file = currentFile_;
    completed = completed_;
    total = total_;
  }

  const float ratio = total > 0 ? static_cast<float>(static_cast<double>(completed) /
                                                     static_cast<double>(total))
                                : 0.0f;
  bar_->setProgress(ratio);

  std::wstring detail = std::to_wstring(total > 0 ? completed * 100 / total : 0) + L"%";
  if (total > 0) {
    detail += L"   " + util::FormatSize(completed) + L" / " + util::FormatSize(total);
  } else if (completed > 0) {
    detail += L"   " + util::FormatSize(completed);
  }
  if (detailLabel_) detailLabel_->setText(detail);
  if (fileLabel_ && !file.empty()) {
    fileLabel_->setText(L"当前文件：" + util::BaseName(file));
  }
}

void DlgProgress::finishWork() {
  finished_.store(true);
  applySnapshot();
  close();
}

void DlgProgress::onCreate() {
  setEscapeCloses(true);
  setTitle(title_);

  fileLabel_ = findView<AiUI::TextView>("file");
  detailLabel_ = findView<AiUI::TextView>("detail");
  bar_ = findView<AiUI::ProgressBar>("bar");
  cancelButton_ = findView<AiUI::Button>("btnCancel");

  if (cancelButton_) {
    cancelButton_->setOnClick([this] {
      cancel_.store(true);
      // 取消后按钮置灰，避免连点；文字改成「正在取消」给用户反馈
      cancelButton_->setEnabled(false);
      cancelButton_->setLabel(L"正在取消…");
    });
  }

  startWork();
}

bool DlgProgress::onCloseRequest() {
  // 关窗等同取消，但窗口要等 worker 退出后才能销毁
  if (finished_.load()) return true;
  cancel_.store(true);
  return false;
}

void DlgProgress::onDestroy() {
  // 必须在窗口销毁前回收工作线程：它还会读 this
  cancel_.store(true);
  if (worker_.joinable()) worker_.join();
  // 之后到达的投递回调直接丢弃，不再碰已经销毁的控件
  alive_->store(false);
}

}  // namespace

bool ShowProgressDialog(AiUI::NativeHandle owner, const std::wstring& title, const WorkFn& work) {
  DlgProgress dlg(title, work);
  dlg.runModal(owner, 420, 190);
  return dlg.succeeded();
}

void RunOnUiThread(const std::function<void()>& fn) {
  if (!fn) return;
  AiUI::platform()->invokeOnUiThread(fn);
}

}  // namespace ui
