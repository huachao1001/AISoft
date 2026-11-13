// 进度窗口
//
// 工作线程只更新一份加锁的快照并请求刷新；控件只在 UI 线程上碰。
// 取消用原子标志，工作线程的进度回调读它返回 false 即可中断。
//
// 外观：窗口没有标题栏，整个客户区就是一条大进度条——左侧压着文件信息，
// 进度填充从左边一路铺过去。鼠标停在整条上都能拖动窗口。
#include "dlg_progress.h"

#include "util.h"
#include "AiUI/theme.h"
#include "AiUI/view_group.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/form_controls.h"
#include "AiUI/widgets/text_view.h"
#include "dialog_activity.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace ui {
namespace {

// 收尾动画的节奏：每帧间隔与最短展示时长。
// 任务越小越容易"闪一下"，留出这段时间让人看清进度条走完。
constexpr int kLandingFrameMs = 30;
constexpr int64_t kMinVisibleMs = 900;

// 定时器载体：View 才有 onDelayed，而 Activity 不是 View。
// 这个薄壳挂在根视图上，唯一的职责是把延时回调转给进度窗口。
// postDelayed 是 protected，只能从 View 内部调用，因此在这里开一个口子。
class TickView : public AiUI::View {
 public:
  explicit TickView(std::function<void()> tick) : tick_(std::move(tick)) {}

  // 重新计时；只能在视图已挂到窗口上之后调用
  void schedule(int ms) { postDelayed(ms); }

 protected:
  void onDelayed() override {
    if (tick_) tick_();
  }

 private:
  std::function<void()> tick_;
};

class DlgProgress : public DialogActivity {
 public:
  DlgProgress(std::wstring title, std::wstring targetName, WorkFn work)
      : title_(std::move(title)), targetName_(std::move(targetName)), work_(std::move(work)) {}
  ~DlgProgress() override;

  bool succeeded() const { return succeeded_.load(); }

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
  // UI 线程：工作结束；可能先补完进度动画，再真正关闭
  void finishWork();
  // UI 线程：补完动画的一帧
  void stepLanding();
  // 现在是否可以离开：至少展示够时长，且进度已经走到 100%
  bool landingDone() const;
  // 用户要求中止：先确认，再置取消标志
  void requestCancel();
  // 主行文案：动作前缀 + 目标包名（不随进度变化）
  std::wstring fileLine() const;
  // 副行文案：百分比 + 大小 + 当前成员
  std::wstring detailLine(uint64_t completed, uint64_t total, const std::wstring& current) const;
  bool progressCallback(uint64_t completed, uint64_t total, const std::wstring& current);

  std::wstring title_;
  // 目标包名：压缩是输出包、解压是输入包。主行显示它，不随进度变化。
  std::wstring targetName_;
  WorkFn work_;

  AiUI::TextView* fileLabel_ = nullptr;
  AiUI::TextView* detailLabel_ = nullptr;
  AiUI::ProgressBar* bar_ = nullptr;
  AiUI::Button* cancelButton_ = nullptr;

  std::atomic<bool> cancel_{false};
  std::atomic<bool> finished_{false};
  // 确认框开着时不再响应第二次点击
  bool askingCancel_ = false;
  // 投递的回调可能在对话框销毁后才被消息循环取出；共享标志用来判断是否还该碰控件
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  // 工作线程写、UI 线程读，必须是原子量
  std::atomic<bool> succeeded_{false};

  // 窗口出现时刻：用来算"已经展示了多久"，短任务也要留够可读时间
  int64_t shownAtMillis_ = 0;
  // 收尾动画的定时器载体：View 才有延时回调
  TickView* tickView_ = nullptr;
  // 工作已结束、正在补完进度动画
  bool landing_ = false;
  // 结束时的快照，补动画期间保持不变（副行数字与成员名不能跳）
  uint64_t landingCompleted_ = 0;
  uint64_t landingTotal_ = 0;
  std::wstring landingFile_;

  std::mutex snapshotMutex_;
  std::wstring currentFile_;
  uint64_t completed_ = 0;
  uint64_t total_ = 0;
  int64_t lastPostMillis_ = 0;
  // 已显示的最大比例：进度条只许前进，不能因为换趟计数而回退
  float shownRatio_ = 0.0f;

  std::thread worker_;
};

DlgProgress::~DlgProgress() {
  // 置位后再 join：窗口若没走正常关闭流程（例如创建失败），已排队的
  // 投递回调必须作废，否则会去碰已析构的 this。
  alive_->store(false);
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
    succeeded_.store(ok);
    // 收尾必须回到 UI 线程：控件只能在那里销毁
    auto alive = alive_;
    AiUI::platform()->postToUiThread([this, alive] {
      if (alive->load()) finishWork();
    });
  });
}

void DlgProgress::applySnapshot() {
  if (!bar_) return;
  // 收尾阶段由 stepLanding 独占刷新：此时真实快照已经停在结束值，
  // 再让排队的回调写一次会和补动画的数字打架。
  if (landing_) return;

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
  // 归档层已经保证完成量单调，这里再兜一层：比例只许前进，
  // 免得多趟编码/解压换成别的路径时又从 100% 掉回去。
  if (ratio > shownRatio_) shownRatio_ = ratio;
  bar_->setProgress(shownRatio_);

  // 副行：百分比 + 大小，尾部再跟当前正在处理的成员。
  // 百分比取自与进度条同一个 shownRatio_，两边永远不会对不上。
  if (detailLabel_) detailLabel_->setText(detailLine(completed, total, file));

  // 主行是固定目标：只有包名，不随处理进度变化
  if (fileLabel_) fileLabel_->setText(fileLine());
}

// 副行：百分比 + 大小，尾部再跟当前正在处理的成员。
// 百分比与进度条同源，永远对得上；主行留给包名，这里才需要滚动变化。
std::wstring DlgProgress::detailLine(uint64_t completed, uint64_t total,
                                     const std::wstring& current) const {
  std::wstring detail = std::to_wstring(static_cast<int>(shownRatio_ * 100.0f + 0.5f)) + L"%";
  if (total > 0) {
    detail += L"  ·  " + util::FormatSize(completed) + L" / " + util::FormatSize(total);
  } else if (completed > 0) {
    detail += L"  ·  " + util::FormatSize(completed);
  }
  if (!current.empty()) detail += L"  ·  " + util::BaseName(current);
  return detail;
}

// 主行写成「正在压缩：xxx.zip」——窗口没有标题栏，动作与目标都靠这一行说明。
// 包名来自调用方：压缩时是输出包，解压时是输入包。
std::wstring DlgProgress::fileLine() const {
  const std::wstring name =
      targetName_.empty() ? std::wstring(L"正在准备…") : util::BaseName(targetName_);
  if (title_.empty()) return name;
  return title_ + L"：" + name;
}

void DlgProgress::requestCancel() {
  if (askingCancel_ || finished_.load()) return;

  // 收尾阶段任务已经跑完，只是还在补进度动画。这时点关闭不该问"是否取消"，
  // 否则用户会以为自己的数据被丢掉了。
  if (landing_) {
    finished_.store(true);
    close();
    return;
  }

  askingCancel_ = true;

  AiUI::MessageBoxParams params;
  params.title = title_;
  params.message = L"确定要取消吗？已处理的内容不会保留。";
  params.icon = AiUI::MessageBoxIcon::kWarning;
  params.buttons = AiUI::MessageBoxButtons::kYesNo;
  // 默认停在「否」：误触关闭不该丢掉已完成的进度
  params.defaultButton = AiUI::MessageBoxResult::kNo;
  const auto answer = AiUI::platform()->showMessageBox(window()->handle(), params);
  askingCancel_ = false;

  if (answer != AiUI::MessageBoxResult::kYes || finished_.load()) return;
  cancel_.store(true);
  // 确认后按钮置灰，避免连点
  if (cancelButton_) cancelButton_->setEnabled(false);
}

void DlgProgress::finishWork() {
  // 失败或用户取消：不该补满进度，否则看着像成功了。直接关窗，
  // 后续由调用方弹错误框说明情况。
  if (!succeeded_) {
    finished_.store(true);
    close();
    return;
  }

  // 成功但窗口可能刚出现几十毫秒：小文件会"闪一下"就没了。
  // 这里不立刻关闭，先进入收尾阶段，把进度条补到 100% 并保证可读时长。
  landing_ = true;

  // 固定结束时的快照：补动画期间副行的大小与成员名不能变
  {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    landingCompleted_ = completed_;
    landingTotal_ = total_;
    landingFile_ = currentFile_;
  }

  stepLanding();
}

// 收尾阶段的一帧：把显示比例推向 100%，同时守住最短展示时长
void DlgProgress::stepLanding() {
  if (!landing_) return;

  // 已经够久且进度走满，才真正关闭
  if (landingDone()) {
    finished_.store(true);
    close();
    return;
  }

  // 把剩余进度摊到剩余展示时间里：任务早就结束时进度条匀速走完，
  // 正好在最短展示时长那一刻到 100%，而不是先冲满再干等。
  const int64_t elapsed = AiUI::platform()->monotonicMillis() - shownAtMillis_;
  const int64_t remaining = kMinVisibleMs - elapsed;
  if (shownRatio_ < 1.0f) {
    // 长任务早就超过最短时长，remaining<=0，直接补齐到 100%
    float step = 1.0f;
    if (remaining > 0) {
      const float framesLeft = static_cast<float>(remaining) / kLandingFrameMs;
      step = (1.0f - shownRatio_) / (framesLeft > 1.0f ? framesLeft : 1.0f);
    }
    shownRatio_ = shownRatio_ + step > 1.0f ? 1.0f : shownRatio_ + step;
    if (bar_) bar_->setProgress(shownRatio_);
    // 副行与正常刷新共用一份拼装，收尾时数字和文件名的格式不会跳变。
    // 收尾阶段不再换文件，用结束时的成员名。
    if (detailLabel_)
      detailLabel_->setText(detailLine(landingCompleted_, landingTotal_, landingFile_));
  }

  if (tickView_) tickView_->schedule(kLandingFrameMs);
}

// 可以离开的条件：窗口已展示够最短时长，且进度条已经到 100%。
// 两者都满足才能关闭，否则短任务仍会一闪而过。
bool DlgProgress::landingDone() const {
  if (shownRatio_ < 1.0f) return false;
  const int64_t elapsed = AiUI::platform()->monotonicMillis() - shownAtMillis_;
  return elapsed >= kMinVisibleMs;
}

void DlgProgress::onCreate() {
  setEscapeCloses(true);
  setTitle(title_);

  // 整个窗口就是进度条，不要标题栏
  if (AiUI::Window* win = window()) {
    win->setTitleBarVisible(false);
    // 没有标题栏就没有可拖区域，把内容区整体当成标题栏
    if (AiUI::View* content = contentView()) content->setDragWindow(true);
  }

  fileLabel_ = findView<AiUI::TextView>("file");
  detailLabel_ = findView<AiUI::TextView>("detail");
  bar_ = findView<AiUI::ProgressBar>("bar");
  cancelButton_ = findView<AiUI::Button>("btnCancel");

  // 配色照参考图：整车极浅的淡蓝，进度填充只比轨道深一点，
  // 对比靠深藏青标题拉开，而不是靠色块。深色主题下换一组同结构配色，
  // 否则一块浅蓝底会突兀地亮在深色界面里。
  const AiUI::Theme& t = bar_ ? bar_->theme() : AiUI::defaultTheme();
  const int luma = (AiUI::colorRed(t.body) * 299 + AiUI::colorGreen(t.body) * 587 +
                    AiUI::colorBlue(t.body) * 114) /
                   1000;
  const bool dark = luma < 128;

  const AiUI::Color trackColor = dark ? AiUI::makeColor(0x24, 0x2b, 0x36)
                                      : AiUI::makeColor(0xe8, 0xef, 0xfa);
  const AiUI::Color fillTop = dark ? AiUI::makeColor(0x31, 0x3d, 0x52)
                                   : AiUI::makeColor(0xcf, 0xdf, 0xf5);
  const AiUI::Color fillBottom = dark ? AiUI::makeColor(0x38, 0x46, 0x5e)
                                      : AiUI::makeColor(0xdd, 0xe9, 0xf8);
  const AiUI::Color titleInk = dark ? AiUI::makeColor(0xe4, 0xec, 0xfa)
                                    : AiUI::makeColor(0x1a, 0x3a, 0x6b);
  const AiUI::Color detailInk = dark ? AiUI::makeColor(0x9f, 0xb4, 0xd0)
                                     : AiUI::makeColor(0x4a, 0x6b, 0x95);

  if (bar_) {
    AiUI::ProgressSpec spec = bar_->spec();
    spec.track = trackColor;
    spec.fillTop = fillTop;
    spec.fillBottom = fillBottom;
    bar_->setSpec(spec);
    bar_->setProgress(0.0f);
  }

  if (fileLabel_) fileLabel_->setColor(titleInk);
  if (detailLabel_) detailLabel_->setColor(detailInk);

  if (cancelButton_) {
    // 只有图标：幽灵样式，任何状态都不铺底，悬停只把线条染红
    AiUI::ButtonSpec spec = cancelButton_->spec();
    spec.style = AiUI::ButtonStyle::kGhost;
    spec.radius = 0;
    spec.padX = 0;
    spec.background = AiUI::kNoColor;
    spec.border = AiUI::kNoColor;
    spec.ink = titleInk;
    spec.hoverBackground = AiUI::kNoColor;
    spec.downBackground = AiUI::kNoColor;
    spec.hoverInk = dark ? AiUI::makeColor(0xff, 0x8a, 0x8a) : AiUI::makeColor(0xc0, 0x1c, 0x28);
    spec.cursor = AiUI::CursorShape::kArrow;
    cancelButton_->setSpec(spec);

    cancelButton_->setOnClick([this] { requestCancel(); });
  }

  // 首帧就把主行画出来：包名一开始就知道，不必等工作线程首个回调
  if (fileLabel_) fileLabel_->setText(fileLine());
  if (detailLabel_) detailLabel_->setText(L"0%");

  // 记录出现时刻：小文件几毫秒就跑完，靠它判断是否还需要留时间给眼睛
  shownAtMillis_ = AiUI::platform()->monotonicMillis();

  // 定时器载体：View 才有延时回调，Activity 没有。
  // 0×0 固定尺寸，不参与布局，只承接 onDelayed。
  tickView_ = new TickView([this] { stepLanding(); });
  if (AiUI::View* content = contentView()) {
    if (auto* group = dynamic_cast<AiUI::ViewGroup*>(content)) {
      group->addView(tickView_, AiUI::LayoutParams::fixed(0, 0));
    }
  }

  startWork();
}

bool DlgProgress::onCloseRequest() {
  // 关窗等同取消，但窗口要等 worker 退出后才能销毁。
  // 已结束（含收尾动画中）就放行；否则先弹确认，拒绝时保持窗口。
  if (finished_.load() || landing_) return true;
  requestCancel();
  return finished_.load();
}

void DlgProgress::onDestroy() {
  // 必须在窗口销毁前回收工作线程：它还会读 this
  cancel_.store(true);
  if (worker_.joinable()) worker_.join();
  // 之后到达的投递回调直接丢弃，不再碰已经销毁的控件
  alive_->store(false);
}

}  // namespace

bool ShowProgressDialog(AiUI::NativeHandle owner, const std::wstring& title,
                        const std::wstring& targetName, const WorkFn& work) {
  DlgProgress dlg(title, targetName, work);
  // 参考图的比例：约 3:1 的长条，两行文字上下排开
  dlg.runModal(owner, 560, 176);
  return dlg.succeeded();
}
void RunOnUiThread(const std::function<void()>& fn) {
  if (!fn) return;
  AiUI::platform()->invokeOnUiThread(fn);
}

}  // namespace ui
