// 通用安装界面实现
//
// 界面只负责：把清单文案贴上去、收集目录与范围、展示结果。
// 落盘、配置、快捷方式、卸载全在 AiInstaller 的 install.cpp 里。
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
  const std::wstring error = install();
  if (!error.empty()) {
    showMessage(error, true);
    return;
  }
  // 装完后要跑什么命令由清单的 [run] afterInstall 决定，runInstall(plan)
  // 已经执行过；这里只负责关界面（提权重启时 install() 也会走到这里）。
  close();
}

std::wstring SetupActivity::install() {
  const Scope scope = selectedScope();
  const std::wstring dir = selectedDir();
  if (dir.empty()) return L"请填写安装目录。";

  InstallRequest request;
  request.app = app_;
  request.targetDir = dir;
  request.scope = scope;

  // 所有用户必须提权：Program Files 与 HKLM 都写不了。
  // 提权后的新进程带 --elevated-retry 自动继续，用户只需在 UAC 点一次「是」。
  if (needsElevation(scope)) {
    if (relaunchElevated(request)) {
      close();
      return {};
    }
    return L"需要管理员权限才能安装到「所有用户」。\n\n请右键安装程序，选择「以管理员身份运行」。";
  }

  InstallPlan plan;
  plan.request = request;
  plan.files = manifest_.files;
  plan.configModule = manifest_.configModule;
  // 显式写全名：本类有个同名的 runInstall()，不加限定会被成员函数遮蔽
  return AiInstaller::runInstall(plan);
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

bool SetupActivity::onCloseRequest() { return true; }

}  // namespace ui
}  // namespace AiInstaller
