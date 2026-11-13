// 外壳集成的通用层：把应用的动作清单与托盘回调转给平台后端
#include "AiUI/shell.h"

#include "AiUI/platform.h"

namespace AiUI {

namespace {

// 本次进程的安装范围。安装程序提权后置为「所有用户」，
// 由它在注册前显式设置，普通启动路径保持默认。
InstallScope g_installScope = InstallScope::kCurrentUser;

}  // namespace

InstallScope currentInstallScope() { return g_installScope; }

void setCurrentInstallScope(InstallScope scope) { g_installScope = scope; }

std::wstring registerShellVerbs(const std::vector<ShellVerb>& verbs) {
  return platform()->registerShellVerbs(verbs);
}

std::wstring unregisterShellVerbs(const std::vector<ShellVerb>& verbs) {
  return platform()->unregisterShellVerbs(verbs);
}

bool areShellVerbsRegistered(const std::vector<ShellVerb>& verbs) {
  return platform()->areShellVerbsRegistered(verbs);
}

std::wstring registerShellMenuHandler(const ShellMenuHandler& handler) {
  // 未显式指定范围时跟随当前进程范围，安装程序与普通启动都能用同一份调用
  if (!handler.scopeExplicit) {
    ShellMenuHandler scoped = handler;
    scoped.scope = currentInstallScope();
    scoped.scopeExplicit = true;
    return platform()->registerShellMenuHandler(scoped);
  }
  return platform()->registerShellMenuHandler(handler);
}

std::wstring unregisterShellMenuHandler(const ShellMenuHandler& handler) {
  if (!handler.scopeExplicit) {
    ShellMenuHandler scoped = handler;
    scoped.scope = currentInstallScope();
    scoped.scopeExplicit = true;
    return platform()->unregisterShellMenuHandler(scoped);
  }
  return platform()->unregisterShellMenuHandler(handler);
}

bool isShellMenuHandlerRegistered(const ShellMenuHandler& handler) {
  if (!handler.scopeExplicit) {
    ShellMenuHandler scoped = handler;
    scoped.scope = currentInstallScope();
    scoped.scopeExplicit = true;
    return platform()->isShellMenuHandlerRegistered(scoped);
  }
  return platform()->isShellMenuHandlerRegistered(handler);
}

// 嵌套类可以访问 TrayIcon 的私有成员，托盘事件就能直接回到应用回调
class TrayIcon::Host final : public PlatformTrayHost {
 public:
  explicit Host(TrayIcon& owner) : owner_(owner) {}

  void onTrayActivate() override {
    if (owner_.onActivate_) owner_.onActivate_();
  }

  void onTrayMenuCommand(int index) override {
    if (owner_.menuHandler_) owner_.menuHandler_(index);
  }

  std::vector<MenuItem> trayMenu() override {
    return owner_.menuProvider_ ? owner_.menuProvider_() : std::vector<MenuItem>{};
  }

 private:
  TrayIcon& owner_;
};

TrayIcon::TrayIcon() = default;

TrayIcon::~TrayIcon() { hide(); }

bool TrayIcon::ensureTray() {
  if (tray_) return true;
  if (!host_) host_ = std::make_unique<Host>(*this);
  tray_ = platform()->createTrayIcon(*host_);
  return tray_ != nullptr;
}

void TrayIcon::show(const std::wstring& tooltip) {
  if (!ensureTray()) return;
  visible_ = tray_->show(tooltip);
}

void TrayIcon::hide() {
  if (!tray_) return;
  tray_->hide();
  visible_ = false;
}

void TrayIcon::setTooltip(const std::wstring& tooltip) {
  if (tray_) tray_->setTooltip(tooltip);
}

}  // namespace AiUI
