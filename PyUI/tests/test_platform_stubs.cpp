// 测试用的平台桩
//
// 控件层不碰平台，但 TitleBar 构造时按设计取「本平台标题栏外观」，
// 那是 src/os/<platform>/ 提供的。窗口后端还没落地，所以测试里给一份
// Windows 基准值——不是「假装平台存在」，而是把 TitleBar 里唯一一处
// 平台依赖固定在已知数字上，断开之后外观断言才能写死期望值。
//
// 这个文件只被测试链接，**绝不能**被应用链接：真机上应该由
// src/os/windows/ 提供同一函数。
#include "pyui/platform/title_bar.h"

namespace pyui {

TitleBarAppearance titleBarAppearance() {
  TitleBarAppearance appearance;
  // Windows 10/11 的取值：32 逻辑高、46×32 的按钮热区、按钮在右
  appearance.bandHeight = 32;
  appearance.visualHeight = 0;
  appearance.buttonsOnSide = TitleButtonSide::kRight;
  appearance.buttonWidth = 46;
  appearance.buttonHeight = 32;
  appearance.roundButtons = false;
  appearance.contentInset = 10;
  appearance.iconTextGap = 8;
  appearance.iconSize = 11;     // 按钮字形
  appearance.appIconSize = 18;  // 应用图标（位图，比字形大一档）
  appearance.titleFontFamily = nullptr;
  appearance.titlePointSize = 9.0f;
  appearance.titleBold = false;
  appearance.drawBottomLine = true;
  return appearance;
}

}  // namespace pyui
