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
const wchar_t* kDisplayName = L"AIZip";

// 让已落地的程序完成右键菜单注册。组件是它自己的内嵌资源，安装器不重复实现。
const wchar_t* kRegisterArg = L"--register-shell-menu";

const wchar_t* kTitle = L"AIZip";

// 标记文件：安装版把右键组件放在程序目录，程序靠它区分「安装版」与开发期构建
const wchar_t* kInstalledMarker = L"installed.marker";
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
  setTitle(L"安装 AIZip");

  scopeGroup_ = findView<AiUI::RadioGroup>("scopeGroup");
  dirBox_ = findView<AiUI::InputBox>("dir");
  elevateHint_ = findView<AiUI::TextView>("elevateHint");

  if (scopeGroup_) {
    scopeGroup_->setOnChange([this](int index) {
      applyScope(index == 1 ? AiUI::InstallScope::kAllUsers : AiUI::InstallScope::kCurrentUser,
                 true);
    });
    // 默认「仅当前用户」：不需要提权，装完立刻可用
    scopeGroup_->setCheckedIndex(0);
  }

  applyScope(AiUI::InstallScope::kCurrentUser, true);

  if (auto* browseButton = findView<AiUI::Button>("btnBrowse")) {
    browseButton->setOnClick([this] { browse(); });
  }
  if (auto* installButton = findView<AiUI::Button>("btnInstall")) {
    installButton->setOnClick([this] {
      const std::wstring error = install();
      if (!error.empty()) {
        messageBox(window()->handle(), error, AiUI::MessageBoxIcon::kError);
        return;
      }
      messageBox(window()->handle(),
                 L"安装完成。\n\n在资源管理器里右键文件或文件夹即可看到 AIZip 菜单。",
                 AiUI::MessageBoxIcon::kInfo);
      close();
    });
  }
  if (auto* cancel = findView<AiUI::Button>("btnCancel")) {
    cancel->setOnClick([this] { close(); });
  }
}

AiUI::InstallScope InstallActivity::selectedScope() const {
  // 索引 0 = 当前用户，1 = 所有用户；未选中时退回当前用户
  const int index = scopeGroup_ ? scopeGroup_->checkedIndex() : 0;
  return index == 1 ? AiUI::InstallScope::kAllUsers : AiUI::InstallScope::kCurrentUser;
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

  // 所有用户必须提权：Program Files 与 HKLM 都写不了
  if (scope == AiUI::InstallScope::kAllUsers && !AiUI::isRunningElevated()) {
    if (AiUI::relaunchElevated()) {
      close();
      return {};
    }
    return L"需要管理员权限才能安装到「所有用户」。\n\n请右键安装程序，选择「以管理员身份运行」。";
  }

  // 1. 落盘：内嵌的 AiZip.exe
  const std::vector<AiUI::InstallFile> files = {{kAiZipResourceId, kExeName}};
  const std::wstring writeError = AiUI::installFiles(dir, files);
  if (!writeError.empty()) return writeError;

  // 安装标记：程序据此把右键组件放到自己目录，并按这里的范围注册菜单
  const std::string markerContent = scope == AiUI::InstallScope::kAllUsers
                                        ? "scope=all-users\r\n"
                                        : "scope=current-user\r\n";
  const std::wstring markerError = AiUI::writeInstallMarker(dir, kInstalledMarker, markerContent);
  if (!markerError.empty()) return markerError;

  // 2. 注册右键菜单：让装好的程序自己做，组件是它的内嵌资源
  const std::wstring exePath = dir + L"\\" + kExeName;
  const int exitCode = AiUI::runAndWait(exePath, kRegisterArg);
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
