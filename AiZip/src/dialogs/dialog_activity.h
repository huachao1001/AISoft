// 对话框 Activity 的公共基类
//
// 五个对话框共同的形状：XML 布局 + 底部「确定/取消」+ 模态运行。
// 基类负责窗口样式、居中与结果回传，子类只声明布局名与业务。
#pragma once

#include "AiUI/activity.h"
#include "AiUI/platform.h"

#include <string>

namespace ui {

class DialogActivity : public AiUI::Activity {
 public:
  // 调用方的主窗口句柄；对话框自身关闭后仍需要它给后续弹窗做 owner
  void setOwnerWindow(AiUI::NativeHandle owner) { ownerWindow_ = owner; }
  // 模态运行；返回窗口关闭前是否点过「确定」
  bool runModal(AiUI::NativeHandle owner, int width, int height);
  bool accepted() const { return accepted_; }

 protected:
  // 居中并换算成物理像素后返回客户区位置
  AiUI::Rect centeredBounds(int width, int height) const;
  // 给后续弹窗用的 owner：对话框已关闭时退回主窗口
  AiUI::NativeHandle ownerWindow() const;

  void setAccepted(bool accepted) { accepted_ = accepted; }
  // 点「确定」时调用；返回 false 表示校验失败、不关闭
  virtual bool onAccept() { return true; }

  bool onCloseRequest() override;

 private:
  AiUI::NativeHandle ownerWindow_ = nullptr;
  bool accepted_ = false;
};

}  // namespace ui
