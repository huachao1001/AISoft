// 通用安装界面
//
// 这类只做装配：把清单里的文案贴到固定布局上，收集「装到哪、什么范围」，
// 真正动手的是 AiInstaller（解压 → 调配置模块 → 快捷方式 → 卸载入口）。
// 界面里没有任何应用私有知识，换应用只换清单。
#pragma once

#include "AiInstaller/app_info.h"
#include "AiInstaller/install.h"
#include "AiInstaller/manifest.h"
#include "AiUI/activity.h"
#include "AiUI/widgets/form_controls.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/text_view.h"

#include <string>

namespace AiInstaller {
namespace ui {

class SetupActivity : public AiUI::Activity {
 public:
  const wchar_t* layoutName() const override { return L"dlg_setup"; }

  // 清单来自安装包；界面的文案、图标与安装内容全部由它决定
  void setManifest(const Manifest& manifest);

  // 已解到临时文件的 logo 路径。图片只能按路径解码，解包由入口完成。
  void setLogoPath(const std::wstring& path) { logoPath_ = path; }

  // 提权后由新进程直接执行安装，不再要求用户点第二次「安装」
  void setAutoInstall(const std::wstring& dir, Scope scope);

  // 卸载通道：不建界面，执行后退出
  void setAutoUninstall(const std::wstring& installDir, const Manifest& manifest);

  // 需要立刻结束（如「这不是安装包」）：建好窗口后直接关掉
  void setAbortWith(const std::wstring& message);

 protected:
  void onCreate() override;
  bool onCloseRequest() override;

 private:
  Scope selectedScope() const;
  std::wstring selectedDir() const;
  void applyScope(Scope scope, bool forceDir);
  void browse();

  // 落盘、配置、快捷方式；成功返回空串
  std::wstring install();
  void runInstall();
  void runUninstall();

  void showMessage(const std::wstring& text, bool error);

  AiUI::RadioGroup* scopeGroup_ = nullptr;
  AiUI::InputBox* dirBox_ = nullptr;
  AiUI::TextView* hint_ = nullptr;

  Manifest manifest_;
  AppInfo app_;
  std::wstring logoPath_;

  bool autoInstall_ = false;
  std::wstring autoDir_;
  Scope autoScope_ = Scope::kAllUsers;

  bool autoUninstall_ = false;
  std::wstring uninstallDir_;

  bool abort_ = false;
  std::wstring abortMessage_;
};

}  // namespace ui
}  // namespace AiInstaller
