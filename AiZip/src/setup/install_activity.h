// 安装界面
//
// 界面只收集「装到哪、什么范围」，真正动手在 InstallActivity::install()：
// 落盘 → 注册菜单 → 快捷方式，任一步失败都停下来报原因。
#pragma once

#include "AiUI/activity.h"
#include "AiUI/install.h"
#include "AiUI/widgets/form_controls.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/text_view.h"

#include <string>

namespace ui {

class InstallActivity : public AiUI::Activity {
 public:
  const wchar_t* layoutName() const override { return L"dlg_install"; }

  // 提权后由新进程直接执行安装，不再要求用户点第二次「安装」。
  // 参数由提权前的实例通过命令行带过来。
  void setAutoInstall(const std::wstring& dir, AiUI::InstallScope scope);

 protected:
  void onCreate() override;
  bool onCloseRequest() override;

 private:
  // 界面 → 状态
  AiUI::InstallScope selectedScope() const;
  std::wstring selectedDir() const;

  // 切换范围时把目录更新成该范围的默认值
  void applyScope(AiUI::InstallScope scope, bool forceDir);

  // 依次落盘、注册、建快捷方式；成功返回空串
  std::wstring install();

  void browse();

  // 真正执行安装并把结果落到界面上；失败弹框，成功打开程序后退出
  void runInstall();

  AiUI::RadioGroup* scopeGroup_ = nullptr;
  AiUI::InputBox* dirBox_ = nullptr;
  AiUI::TextView* elevateHint_ = nullptr;

  bool autoInstall_ = false;
  std::wstring autoDir_;
  AiUI::InstallScope autoScope_ = AiUI::InstallScope::kAllUsers;
};

}  // namespace ui
