// Windows 标题栏默认度量
//
// 数字取自系统标题栏实测（96 DPI 下按钮约 46x30、图标墨迹约 10x10），
// 保证自绘标题栏与原生窗口观感一致。按钮在右侧。
#include "AiUI/widgets/title_bar.h"

namespace AiUI {

const TitleBarDefaults& titleBarDefaults() {
  static const TitleBarDefaults kDefaults = [] {
    TitleBarDefaults d;
    d.bandHeight = 30;
    d.buttonWidth = 46;
    d.iconSize = 10;
    d.appIconSize = 16;
    d.padLeft = 10;
    // 右侧不留内边距：关闭按钮的悬停底色要一直贴到窗口右边沿
    d.padRight = 0;
    d.iconGap = 8;
    d.buttonsOnLeft = false;
    return d;
  }();
  return kDefaults;
}

}  // namespace AiUI
