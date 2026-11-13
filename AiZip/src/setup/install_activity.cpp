// 安装界面实现
#include "install_activity.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/form_controls.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/option_view.h"
#include "AiUI/widgets/text_view.h"

namespace ui {
namespace {

// 内嵌资源 id 与落地文件名。资源由 CMake 打进本程序（RCDATA）。
constexpr int kAiZipResourceId = 101;
const wchar_t* kExeName = L"AiZip.exe";
const wchar_t* kDisplayName = L"爱压";

const wchar_t* kTitle = L"爱压";
void messageBox(AiUI::NativeHandle owner, const std::wstring& text,
                AiUI::MessageBoxIcon icon) {
  AiUI::MessageBoxParams params;
  params.title = kTitle;
  params.message = text;
  params.icon = icon;
  AiUI::platform()->showMessageBox(owner, params);
}

}  // namespace

void InstallActivity::onCreate() {
  setTitle(L"安装爱压");

  scopeGroup_ = findView<AiUI::RadioGroup>("scopeGroup");
  dirBox_ = findView<AiUI::InputBox>("dir");
  elevateHint_ = findView<AiUI::TextView>("elevateHint");

  if (scopeGroup_) {
    scopeGroup_->setOnChange([this](int index) {
      applyScope(index == 1 ? AiUI::InstallScope::kAllUsers : AiUI::InstallScope::kCurrentUser,
                 true);
    });
    // 默认「所有用户」：装一次全机可用，省得每个账户再装一遍
    scopeGroup_->setCheckedIndex(1);
  }

  applyScope(AiUI::InstallScope::kAllUsers, true);

  if (auto* browseButton = findView<AiUI::Button>("btnBrowse")) {
    browseButton->setOnClick([this] { browse(); });
  }
  if (auto* installButton = findView<AiUI::Button>("btnInstall")) {
    installButton->setOnClick([this] { runInstall(); });
  }
  if (auto* cancel = findView<AiUI::Button>("btnCancel")) {
    cancel->setOnClick([this] { close(); });
  }

  // 提权后的新进程：按带过来的选择直接装，用户只需在 UAC 上点一次「是」
  if (autoInstall_) {
    if (dirBox_) dirBox_->setText(autoDir_);
    if (scopeGroup_) {
      scopeGroup_->setCheckedIndex(autoScope_ == AiUI::InstallScope::kAllUsers ? 1 : 0);
    }
    applyScope(autoScope_, false);
    runInstall();
  }
}

void InstallActivity::setAutoInstall(const std::wstring& dir, AiUI::InstallScope scope) {
  autoInstall_ = true;
  autoDir_ = dir;
  autoScope_ = scope;
}

void InstallActivity::runInstall() {
  const std::wstring error = install();
  if (!error.empty()) {
    messageBox(window()->handle(), error, AiUI::MessageBoxIcon::kError);
    return;
  }
  // 装完直接打开程序，用户不用去开始菜单里找
  const std::wstring exe = selectedDir() + L"\\" + kExeName;
  AiUI::platform()->openWithSystem(exe);
  close();
}

AiUI::InstallScope InstallActivity::selectedScope() const {
  // 索引 0 = 当前用户，1 = 所有用户；未选中时退回默认的「所有用户」
  const int index = scopeGroup_ ? scopeGroup_->checkedIndex() : 1;
  return index == 0 ? AiUI::InstallScope::kCurrentUser : AiUI::InstallScope::kAllUsers;
}

std::wstring InstallActivity::selectedDir() const {
  return dirBox_ ? dirBox_->text() : std::wstring();
}

void InstallActivity::applyScope(AiUI::InstallScope scope, bool forceDir) {
  // 切范围时目录跟着换成该范围的默认值，除非用户已经手动改过
  if (forceDir && dirBox_) {
    const std::wstring dir = AiUI::defaultInstallDir(scope);
    if (!dir.empty()) dirBox_->setText(dir);
  }

  // 所有用户且未提权时给一句提示，别让用户在毫无预告的情况下撞上 UAC
  if (elevateHint_) {
    const bool needElevate =
        scope == AiUI::InstallScope::kAllUsers && !AiUI::isRunningElevated();
    elevateHint_->setText(needElevate
                              ? L"所有用户需要管理员权限，安装时系统会提示提权。"
                              : L"");
    elevateHint_->show(needElevate);
  }
}

void InstallActivity::browse() {
  const std::wstring initial = selectedDir();
  const std::wstring picked = AiUI::platform()->pickFolder(
      window()->handle(), L"选择安装目录", initial);
  if (!picked.empty() && dirBox_) dirBox_->setText(picked);
}

std::wstring InstallActivity::install() {
  const AiUI::InstallScope scope = selectedScope();
  std::wstring dir = selectedDir();
  if (dir.empty()) return L"请填写安装目录。";

  // 所有用户必须提权：Program Files 与 HKLM 都写不了。
  // 提权后的新进程带 --elevated-retry 自动继续，用户只需在 UAC 点一次「是」。
  if (scope == AiUI::InstallScope::kAllUsers && !AiUI::isRunningElevated()) {
    const std::wstring retryArgs =
        L"--elevated-retry --scope=all-users --dir=\"" + dir + L"\"";
    if (AiUI::relaunchElevated(retryArgs)) {
      close();
      return {};
    }
    return L"需要管理员权限才能安装到「所有用户」。\n\n请右键安装程序，选择「以管理员身份运行」。";
  }

  // 1. 落盘：内嵌的 AiZip.exe
  const std::vector<AiUI::InstallFile> files = {{kAiZipResourceId, kExeName}};
  const std::wstring writeError = AiUI::installFiles(dir, files);
  if (!writeError.empty()) return writeError;

  // 2. 注册右键菜单：让装好的程序自己做，组件是它的内嵌资源。
  //    范围必须显式传过去：注册表写 HKLM 还是 HKCU 只有这里知道。
  const std::wstring exePath = dir + L"\\" + kExeName;
  const std::wstring registerArgs =
      scope == AiUI::InstallScope::kAllUsers ? L"--register-shell-menu --scope=all-users"
                                             : L"--register-shell-menu --scope=current-user";
  const int exitCode = AiUI::runAndWait(exePath, registerArgs);
  if (exitCode != 0) {
    return L"右键菜单注册失败（退出码 " + std::to_wstring(exitCode) +
           L"）。\n\n程序已安装到：" + dir;
  }

  // 3. 开始菜单快捷方式；失败不算致命
  AiUI::createShortcuts(dir, kExeName, kDisplayName, scope);
  return {};
}

bool InstallActivity::onCloseRequest() { return true; }

}  // namespace ui
