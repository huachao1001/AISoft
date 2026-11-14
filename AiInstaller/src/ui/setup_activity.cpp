// 通用安装界面实现
//
// 界面只负责：把清单文案贴上去、收集目录与范围、展示进度与结果。
// 落盘、配置、命令、快捷方式、卸载全在 AiInstaller 的 install.cpp 里。
//
// 安装必须跑在工作线程上：runInstall() 会同步复制几十 MB 文件、等注册命令
// 返回，占满 UI 线程期间进度条一格都不会动，窗口也不响应重绘。这里用
// 「工作线程 + 互斥量快照 + 定时取走」的方式更新界面，与 AiZip 的进度
// 对话框同一套路子。
#include "ui/setup_activity.h"

#include "AiInstaller/package.h"
#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/image_view.h"
#include "AiUI/widgets/option_view.h"

#include <string>

namespace AiInstaller {
namespace ui {
namespace {

// 刷新界面的节奏。进度回调可能一步一次，没必要每次都投递窗口消息。
constexpr int kRefreshMs = 60;

}  // namespace

// 定义在头文件里前向声明过的具名类：安装界面需要在成员里持有它。
class TickView : public AiUI::View {
 public:
  explicit TickView(std::function<void()> tick) : tick_(std::move(tick)) {}
  void schedule(int ms) { postDelayed(ms); }
  // 取消已排队的回调。postDelayed 是 protected，只有 View 内部能调。
  void cancel() { postDelayed(0); }

 protected:
  void onDelayed() override {
    if (tick_) tick_();
  }

 private:
  std::function<void()> tick_;
};

void SetupActivity::setManifest(const Manifest& manifest) {
  manifest_ = manifest;

  AppInfo app;
  app.displayName = manifest.displayName;
  app.version = manifest.version;
  app.publisher = manifest.publisher;
  app.exeName = manifest.exeName;
  app.installSubdir = manifest.installSubdir;
  app.afterInstall = manifest.afterInstall;
  app_ = std::move(app);
}

void SetupActivity::setAutoInstall(const std::wstring& dir, Scope scope) {
  autoInstall_ = true;
  autoDir_ = dir;
  autoScope_ = scope;
}

void SetupActivity::setAutoUninstall(const std::wstring& installDir, const Manifest& manifest) {
  autoUninstall_ = true;
  uninstallDir_ = installDir;
  setManifest(manifest);
}

void SetupActivity::setAbortWith(const std::wstring& message) {
  abort_ = true;
  abortMessage_ = message;
}

void SetupActivity::onCreate() {
  // 窗口标题与正文全部来自清单；清单没写就用显示名兜底，避免出现空白标题
  const std::wstring baseTitle =
      manifest_.windowTitle.empty() ? manifest_.displayName : manifest_.windowTitle;
  setTitle(baseTitle);

  if (auto* view = findView<AiUI::TextView>("title")) {
    view->setText(manifest_.title.empty() ? manifest_.displayName : manifest_.title);
  }
  if (auto* view = findView<AiUI::TextView>("description")) {
    view->setText(manifest_.description);
    // 清单没写说明时把整行收起来，别留一条空白
    view->show(!manifest_.description.empty());
  }
  if (auto* button = findView<AiUI::Button>("btnInstall")) {
    button->setLabel(manifest_.installText);
  }
  if (auto* button = findView<AiUI::Button>("btnCancel")) {
    button->setLabel(manifest_.cancelText);
  }
  if (auto* button = findView<AiUI::Button>("btnBrowse")) {
    button->setLabel(manifest_.browseText);
  }

  // 清单里的 logo 由入口先解到临时文件再传进来：图片只能按路径解码，
  // 而解包要读安装包本身，那是入口的职责，界面不该自己去开文件。
  if (auto* logo = findView<AiUI::ImageView>("logo")) {
    if (!logoPath_.empty()) {
      logo->loadFile(logoPath_);
      logo->show(logo->hasImage());
    } else {
      logo->show(false);
    }
  }

  scopeGroup_ = findView<AiUI::RadioGroup>("scopeGroup");
  dirBox_ = findView<AiUI::InputBox>("dir");
  hint_ = findView<AiUI::TextView>("hint");
  bar_ = findView<AiUI::ProgressBar>("bar");
  progressText_ = findView<AiUI::TextView>("progressText");
  runAfterBox_ = findView<AiUI::CheckBox>("runAfter");
  btnFinish_ = findView<AiUI::Button>("btnFinish");

  configPage_ = findView<AiUI::View>("configPage");
  progressPage_ = findView<AiUI::View>("progressPage");
  donePage_ = findView<AiUI::View>("donePage");

  // 只支持当前用户的应用不该显示范围选项：选了也装不了
  if (auto* scopeRow = findView<AiUI::View>("scopeRow")) {
    scopeRow->show(manifest_.showScope);
  }
  if (auto* label = findView<AiUI::RadioButton>("scopeUser")) {
    label->setLabel(manifest_.currentUserText);
  }
  if (auto* label = findView<AiUI::RadioButton>("scopeAll")) {
    label->setLabel(manifest_.allUsersText);
  }

  if (scopeGroup_) {
    scopeGroup_->setOnChange([this](int index) {
      applyScope(index == 1 ? Scope::kAllUsers : Scope::kCurrentUser, true);
    });
    // 默认「所有用户」：装一次全机可用，省得每个账户再装一遍
    scopeGroup_->setCheckedIndex(manifest_.showScope ? 1 : 0);
  }

  // 提权重试时目录由命令行带来，不能被默认值覆盖，所以这里只刷提示、不重算目录
  applyScope(manifest_.showScope ? Scope::kAllUsers : Scope::kCurrentUser, !autoInstall_);

  if (auto* browseButton = findView<AiUI::Button>("btnBrowse")) {
    browseButton->setOnClick([this] { browse(); });
  }
  if (auto* installButton = findView<AiUI::Button>("btnInstall")) {
    installButton->setOnClick([this] { runInstall(); });
  }
  if (auto* cancel = findView<AiUI::Button>("btnCancel")) {
    cancel->setOnClick([this] { close(); });
  }
  if (btnFinish_) {
    btnFinish_->setLabel(L"完成");
    btnFinish_->setOnClick([this] { finish(); });
  }

  // 完成页的「立即打开」：清单给了 afterInstall 才有意义，否则整行收起
  if (runAfterBox_) {
    const bool canRun = !manifest_.afterInstall.empty();
    runAfterBox_->show(canRun);
    runAfterBox_->setLabel(manifest_.runAfterText);
    runAfterBox_->setChecked(manifest_.runAfterChecked, false);
  }

  showStage(Stage::kConfig);

  // 不是安装包：直接告知并退出，别让用户对着一个空界面点
  if (abort_) {
    showMessage(abortMessage_, true);
    close();
    return;
  }

  // 卸载不显示安装界面，直接执行后退出
  if (autoUninstall_) {
    runUninstall();
    return;
  }

  // 提权后的新进程：按带过来的选择直接装，用户只需在 UAC 上点一次「是」
  if (autoInstall_) {
    // 顺序要紧：先设范围再设目录。设范围会触发 onChange 重算默认目录，
    // 反过来会被覆盖，装到用户没指定的地方去。
    if (scopeGroup_) scopeGroup_->setCheckedIndex(autoScope_ == Scope::kAllUsers ? 1 : 0);
    // 一定为 true：静默安装不传目录，得按范围算出默认值，否则界面上是个
    // 空字符串，install() 会以「请填写安装目录」失败。
    applyScope(autoScope_, true);
    // 命令行明确带了目录时以它为准，覆盖刚算出的默认值
    if (!autoDir_.empty() && dirBox_) dirBox_->setText(autoDir_);
    runInstall();
  }
}

void SetupActivity::showStage(Stage stage) {
  if (configPage_) configPage_->show(stage == Stage::kConfig);
  if (progressPage_) progressPage_->show(stage == Stage::kProgress);
  if (donePage_) donePage_->show(stage == Stage::kDone);
}

void SetupActivity::setProgress(float fraction, const std::wstring& text) {
  // 进度条只许前进：换步骤、重算比例都不该让用户看到它往后退
  if (fraction > shownFraction_) shownFraction_ = fraction;
  if (bar_) bar_->setProgress(shownFraction_);

  if (progressText_) {
    const int percent = static_cast<int>(shownFraction_ * 100.0f + 0.5f);
    std::wstring line = std::to_wstring(percent) + L"%";
    if (!text.empty()) line += L"  ·  " + text;
    progressText_->setText(line);
  }
}

void SetupActivity::onProgress(float fraction, const wchar_t* stage, void* user) {
  auto* self = static_cast<SetupActivity*>(user);
  if (!self) return;

  // 只写快照，不投递窗口消息：刷新由 UI 线程上的定时器统一取走。
  // 每条进度都投递的话，复制几十个文件就会灌满消息队列，界面反而卡。
  std::lock_guard<std::mutex> lock(self->snapshotMutex_);
  self->snapshotFraction_ = fraction;
  if (stage) self->snapshotStage_ = stage;
  self->snapshotDirty_ = true;
}

void SetupActivity::applyProgress() {
  float fraction = 0.0f;
  std::wstring text;
  {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    if (!snapshotDirty_) return;
    snapshotDirty_ = false;
    fraction = snapshotFraction_;
    text = snapshotStage_;
  }
  setProgress(fraction, text);
}

void SetupActivity::startInstallWork(const InstallRequest& request) {
  // 上一轮（失败后重试）的线程可能已经跑完但还 joinable，直接赋值给
  // std::thread 会走 terminate。先回收再开新线程。
  if (worker_.joinable()) worker_.join();

  working_.store(true);
  {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    resultOk_ = false;
    resultError_.clear();
    snapshotDirty_ = false;
  }
  resultReady_.store(false);
  shownFraction_ = 0.0f;
  showStage(Stage::kProgress);
  setProgress(0.0f, L"正在准备...");

  // 自循环定时器：每 kRefreshMs 取一次快照，装完就停。
  // 用定时器而不是「每条进度都投递」的理由见 onProgress。
  //
  // 它必须挂在布局树上才收得到定时器（postDelayed 依赖 ViewRoot），
  // 但又不该占位：LinearLayout 会给每个可见子视图之间留一个 spacing，
  // 挂上去就等于凭空多出一条缝，切页时整页会往下挪。所以固定 0 尺寸
  // 再隐藏——隐藏的视图不参与 measure/arrange，定时器照常工作。
  if (!tick_) {
    if (auto* group = dynamic_cast<AiUI::ViewGroup*>(contentView())) {
      tick_ = new TickView([this] { tickOnce(); });
      tick_->show(false);
      group->addView(tick_, AiUI::LayoutParams::fixed(0, 0));
    }
  }
  if (tick_) tick_->schedule(kRefreshMs);
  InstallPlan plan;
  plan.request = request;
  plan.files = manifest_.files;
  plan.configModule = manifest_.configModule;
  plan.commands = manifest_.commands;
  plan.progress = &SetupActivity::onProgress;
  plan.progressUser = this;

  installDir_ = request.targetDir;

  // 工作线程不能碰控件，只写快照；收尾一律投递回 UI 线程
  auto alive = alive_;
  worker_ = std::thread([this, plan, alive] {
    const std::wstring error = AiInstaller::runInstall(plan);
    {
      std::lock_guard<std::mutex> lock(snapshotMutex_);
      resultReady_ = true;
      resultOk_ = error.empty();
      resultError_ = error;
    }
    AiUI::platform()->postToUiThread([this, alive] {
      if (!alive->load()) return;
      onWorkFinished();
    });
  });
}

void SetupActivity::tickOnce() {
  if (!working_.load() && resultReady_.load()) {
    // 已经收尾，定时器停掉
    return;
  }
  applyProgress();
  if (tick_ && isOpen()) tick_->schedule(kRefreshMs);
}

void SetupActivity::onWorkFinished() {
  working_.store(false);
  if (tick_) tick_->cancel();  // 停掉定时器

  std::wstring error;
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    ok = resultOk_;
    error = resultError_;
  }

  if (!ok) {
    // 失败不留在进度页：那看着像还在装。回到配置页并说明原因。
    showStage(Stage::kConfig);
    // 进度条归零：下次重试要从 0 开始，留着半截会让人以为接着上次跑
    shownFraction_ = 0.0f;
    applyProgress();
    showMessage(error, true);
    return;
  }
  // 成功：进度条补满再切完成页
  setProgress(1.0f, L"安装完成");
  showStage(Stage::kDone);
}

void SetupActivity::finish() {
  // 「完成」才是真正的收尾：要不要启动程序在这里决定，而不是装完就弹出来
  if (runAfterBox_ && runAfterBox_->isChecked() && !manifest_.afterInstall.empty()) {
    AiInstaller::runAfterInstall(installDir_, manifest_.afterInstall);
  }
  close();
}

void SetupActivity::showMessage(const std::wstring& text, bool error) {
  AiUI::MessageBoxParams params;
  params.title = manifest_.windowTitle.empty() ? manifest_.displayName : manifest_.windowTitle;
  params.message = text;
  params.icon = error ? AiUI::MessageBoxIcon::kError : AiUI::MessageBoxIcon::kInfo;
  AiUI::platform()->showMessageBox(window() ? window()->handle() : nullptr, params);
}

Scope SetupActivity::selectedScope() const {
  const int index = scopeGroup_ ? scopeGroup_->checkedIndex() : 1;
  return index == 1 ? Scope::kAllUsers : Scope::kCurrentUser;
}

std::wstring SetupActivity::selectedDir() const {
  return dirBox_ ? dirBox_->text() : std::wstring();
}

void SetupActivity::applyScope(Scope scope, bool forceDir) {
  // 切范围时目录跟着换成该范围的默认值，除非用户已经手动改过
  if (forceDir && dirBox_) {
    const std::wstring dir = scope == Scope::kAllUsers ? defaultInstallDir(app_)
                                                       : perUserInstallDir(app_);
    if (!dir.empty()) dirBox_->setText(dir);
  }

  // 所有用户且未提权时给一句提示，别让用户在毫无预告的情况下撞上 UAC
  if (hint_) {
    const bool needElevate = manifest_.showScope && needsElevation(scope);
    hint_->setText(needElevate ? L"「所有用户」需要管理员权限，安装时系统会提示提权。" : L"");
    hint_->show(needElevate);
  }
}

void SetupActivity::browse() {
  const std::wstring picked = AiUI::platform()->pickFolder(
      window()->handle(), L"选择安装目录", selectedDir());
  if (!picked.empty() && dirBox_) dirBox_->setText(picked);
}

void SetupActivity::runInstall() {
  const Scope scope = selectedScope();
  const std::wstring dir = selectedDir();
  if (dir.empty()) {
    showMessage(L"请填写安装目录。", true);
    return;
  }

  InstallRequest request;
  request.app = app_;
  request.targetDir = dir;
  request.scope = scope;

  // 所有用户必须提权：Program Files 与 HKLM 都写不了。
  // 提权后的新进程带 --elevated-retry 自动继续，用户只需在 UAC 点一次「是」。
  if (needsElevation(scope)) {
    if (relaunchElevated(request)) {
      close();
      return;
    }
    showMessage(L"需要管理员权限才能安装到「所有用户」。\n\n请右键安装程序，选择「以管理员身份运行」。",
                true);
    return;
  }

  // 装到进度页并在工作线程上跑：同步执行会把 UI 线程占满，进度条画不出来
  startInstallWork(request);
}

void SetupActivity::runUninstall() {
  RemovePlan plan;
  plan.request.app = app_;
  plan.request.targetDir = uninstallDir_;
  // 范围以安装时记录在目录里的为准。不能靠「当前是否提权」猜：
  // 卸载器带 requireAdministrator，几乎总是提权状态。
  plan.request.scope = readInstalledScope(uninstallDir_, Scope::kAllUsers);

  // 删哪些文件以清单为准：装好的 exe 已被裁掉载荷，回答不了这个问题。
  // 只列真正装到磁盘的那些，品牌素材本来就不在用户目录里。
  for (const auto& file : manifest_.files) {
    if (file.install) plan.files.push_back(file.path);
  }
  plan.configModule = manifest_.configModule;

  const std::wstring error = runRemove(plan);
  close();
  if (!error.empty()) showMessage(error, true);
}

bool SetupActivity::onCloseRequest() {
  // 安装进行中不许关窗：工作线程还在读这份对象，关掉会崩。
  // 提示用户等它跑完，而不是假装拦住。
  if (working_.load()) {
    showMessage(L"正在安装，请稍候。", false);
    return false;
  }
  return true;
}

void SetupActivity::onDestroy() {
  // 窗口销毁前必须回收工作线程：它还会调进度回调
  alive_->store(false);
  if (worker_.joinable()) worker_.join();
  if (tick_) {
    tick_->cancel();
    tick_ = nullptr;
  }
}

}  // namespace ui
}  // namespace AiInstaller
