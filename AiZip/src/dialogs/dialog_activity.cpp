// 对话框 Activity 公共实现
#include "dialog_activity.h"

namespace ui {

AiUI::Rect DialogActivity::centeredBounds(int width, int height) const {
  AiUI::Platform* platform = AiUI::platform();
  const float scale = platform->scaleFactor();
  const int w = static_cast<int>(width * scale);
  const int h = static_cast<int>(height * scale);

  const AiUI::Rect work = platform->workArea();
  // 窗口比工作区还大就收进来，否则居中后会有一部分跑到屏幕外
  const int maxW = work.right - work.left;
  const int maxH = work.bottom - work.top;
  const int finalW = w > maxW ? maxW : w;
  const int finalH = h > maxH ? maxH : h;

  const int x = work.left + (maxW - finalW) / 2;
  const int y = work.top + (maxH - finalH) / 2;
  return AiUI::Rect{x, y, x + finalW, y + finalH};
}

bool DialogActivity::runModal(AiUI::NativeHandle owner, int width, int height) {
  if (!ownerWindow_) ownerWindow_ = owner;
  if (!create(owner)) return false;
  setInitialBounds(centeredBounds(width, height));
  showModal(owner);
  return accepted_;
}

AiUI::NativeHandle DialogActivity::ownerWindow() const {
  // 对话框自己已经销毁，不能再用自己的句柄；退回主窗口
  return ownerWindow_;
}

bool DialogActivity::onCloseRequest() {
  // 关窗等同取消：accepted_ 仍为 false，调用方据此判断
  return true;
}

}  // namespace ui
