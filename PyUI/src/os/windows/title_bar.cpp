// Windows 标题栏外观：只回答「本平台的数字与方向」
//
// 这里刻意不实现任何绘制——标题栏的内容（图标、标题、三个按钮）由
// src/widgets/title_bar.cpp 用通用控件拼出来，三平台共用一份。这个文件
// 唯一的职责是把「Windows 上看起来是原生的」翻译成
// TitleBarAppearance 里的字段。
//
// 为什么这里给的是**设计值**而不是缩放后的像素：TitleBar / WindowStyle 的
// 每个尺寸字段都按 96 DPI 设计值定义，由控件内部 px() 换算。若这里先按屏幕
// DPI 缩放，控件会再乘一次，150% 屏上的标题栏就比 captionHeight 高出一半，
// 顶部缩放带也跟着错位。
//
// 为什么不直接量系统标题栏高度：我们画的是**自绘**标题栏，系统的那一条
// 已经被移除了（客户区覆盖到窗口顶部）。所以这里取的是「与系统观感一致」
// 的数值，而不是 SM_CYCAPTION——后者在 150% DPI 下是 33px 并含边框，
// 直接用会让自绘条比视觉上的标题栏高一截。
#include "pyui/platform/title_bar.h"

#include "win_common.h"

namespace pyui {
namespace {

// Windows 标题栏徽标高度 38、按钮 46×38（96 DPI）。
//
// 这几个数字与 AiUI 的 titleBarDefaults() 逐项对齐：两个库共用同一套 Windows
// 外观，并排看时不该能认出哪张截图是哪个库画的。
//
// 按钮高度必须与徽标高度相等，否则悬停底色会在上下各留一条缝——
// Windows 原生的悬停底色是整条贴边的。
constexpr int kDesignBandHeight = 38;
constexpr int kDesignButtonWidth = 46;
constexpr int kDesignButtonHeight = 38;

}  // namespace

TitleBarAppearance titleBarAppearance() {
  TitleBarAppearance appearance;

  appearance.bandHeight = kDesignBandHeight;
  appearance.visualHeight = 0;
  appearance.buttonsOnSide = TitleButtonSide::kRight;
  appearance.buttonWidth = kDesignButtonWidth;
  appearance.buttonHeight = kDesignButtonHeight;
  appearance.roundButtons = false;

  // 同样取自 AiUI：contentInset 12、iconSize 11、appIconSize 18、iconTextGap 8。
  // iconSize 是按钮字形的边长，appIconSize 是左上角应用图标——两者不是同一个
  // 概念，共用一个数会让应用图标缩到看不清。
  appearance.contentInset = 12;
  appearance.iconTextGap = 8;
  appearance.iconSize = 11;
  appearance.appIconSize = 18;

  // 标题用系统界面字体（Segoe UI）：Windows 的窗口标题与正文同族，
  // 只是稍小。写死字体名会在换了系统语言的机器上显示成缺字方块。
  appearance.titleFontFamily = nullptr;
  appearance.titlePointSize = 9.0f;
  appearance.titleBold = false;
  appearance.drawBottomLine = true;

  // 配色按系统深浅色取。刻意不从 Theme 取：标题栏要在「应用尚未设置主题」
  // 时就已经看起来像本平台的原生窗口，这是 TitleBarAppearance 存在的理由。
  if (win32::isSystemDarkMode()) {
    appearance.background = makeColor(0x20, 0x20, 0x20);
    appearance.backgroundInactive = makeColor(0x20, 0x20, 0x20);
    appearance.titleInk = makeColor(0xd4, 0xd4, 0xd4);
    appearance.titleInkInactive = makeColor(0x80, 0x80, 0x80);
    appearance.buttonHover = makeColor(0x33, 0x33, 0x33);
    appearance.buttonDown = makeColor(0x40, 0x40, 0x40);
    appearance.closeHover = makeColor(0xc4, 0x2b, 0x1c);
    appearance.closeDown = makeColor(0xb0, 0x24, 0x17);
    appearance.closeInk = makeColor(0xff, 0xff, 0xff);
    appearance.bottomLine = makeColor(0x3c, 0x3c, 0x3c);
  } else {
    appearance.background = makeColor(0xff, 0xff, 0xff);
    appearance.backgroundInactive = makeColor(0xff, 0xff, 0xff);
    appearance.titleInk = makeColor(0x1a, 0x1a, 0x1a);
    appearance.titleInkInactive = makeColor(0x99, 0x99, 0x99);
    appearance.buttonHover = makeColor(0xe8, 0xe8, 0xe8);
    appearance.buttonDown = makeColor(0xd8, 0xd8, 0xd8);
    appearance.closeHover = makeColor(0xe8, 0x11, 0x23);
    appearance.closeDown = makeColor(0xc5, 0x0e, 0x1d);
    appearance.closeInk = makeColor(0xff, 0xff, 0xff);
    appearance.bottomLine = makeColor(0xe5, 0xe5, 0xe5);
  }

  return appearance;
}

}  // namespace pyui
