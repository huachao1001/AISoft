// 外壳集成的通用层：把应用的动作清单与托盘回调转给平台后端
#include "AiUI/shell.h"

#include "AiUI/platform.h"

namespace AiUI {

std::wstring registerShellVerbs(const std::vector<ShellVerb>& verbs) {
  return platform()->registerShellVerbs(verbs);
}

std::wstring unregisterShellVerbs(const std::vector<ShellVerb>& verbs) {
  return platform()->unregisterShellVerbs(verbs);
}

bool areShellVerbsRegistered(const std::vector<ShellVerb>& verbs) {
  return platform()->areShellVerbsRegistered(verbs);
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
