// 标题栏外观：各平台只提供「数字与方向」，绘制代码共用一份
//
// 为什么只给外观而不给实现：
//   标题栏内容（应用图标、标题文字、最小化/最大化/关闭三个按钮）用通用控件
//   拼装，跨平台视觉一致；真正的平台差异只有四项——高度、按钮在哪一侧、
//   默认配色、标题字体。把这四项收成一个纯数据结构，绘制就只有一份代码。
//
// 反例：让每个平台各自实现一个 TitleBar 控件，会出现三份几乎相同但细节
// 不同的代码，改一处样式要改三遍，且必然慢慢漂移。
#pragma once

#include "pyui/theme.h"
#include "pyui/types.h"

#include <string>

namespace pyui {

// 系统按钮的排列顺序。macOS 的红绿灯在左，Windows/Linux 在右。
enum class TitleButtonSide {
  kRight,  // 最小化、最大化、关闭（Windows / 多数 Linux 桌面）
  kLeft,   // 关闭、最小化、最大化（macOS）
};

struct TitleBarAppearance {
  // 逻辑高度（96 DPI 设计值）：按钮悬停底色高度、图标与标题的垂直居中基准。
  // 系统的可拖动区域高度也用它，保证「看起来能拖的地方确实能拖」。
  int bandHeight = 32;

  // 视觉总高（96 DPI 设计值）。0 表示等于 bandHeight。
  // 多出来的部分留给宿主自绘（例如在标题栏下方叠一条工具栏底纹）。
  int visualHeight = 0;

  TitleButtonSide buttonsOnSide = TitleButtonSide::kRight;

  // 三个系统按钮的宽高（96 DPI 设计值）。macOS 的圆点是正方形；
  // Windows 是 46×32 的矩形热区。这里只给尺寸，形状由控件决定。
  int buttonWidth = 46;
  int buttonHeight = 32;
  bool roundButtons = false;  // macOS 红绿灯

  // 左右内边距：图标距离窗口左边缘的距离
  int contentInset = 10;
  // 图标与标题之间的间距
  int iconTextGap = 8;
  // 系统按钮里的字形边长（96 DPI 设计值）。它比应用图标小一档：
  // 最小化/最大化/关闭的笔画要看起来比窗口图标更轻。
  int iconSize = 11;
  // 应用图标边长（96 DPI 设计值）；0 表示不显示图标（macOS 通常不显示）。
  //
  // 与 iconSize 分开：两者同用一档尺寸时，应用图标会小到看不清——它是一枚
  // 位图，11px 再乘 DPI 只剩十几个物理像素，缩到任务栏图标一半大小。
  // AiUI 也是分开的两个数（11 / 18）。
  int appIconSize = 18;

  // 标题字体
  const char* titleFontFamily = nullptr;  // nullptr = 平台默认界面字体
  float titlePointSize = 9.0f;
  bool titleBold = false;

  // 默认配色（可被 Theme 覆盖，但这是「未换肤时看起来像本平台原生窗口」的基准）
  Color background = makeColor(0xff, 0xff, 0xff);
  Color backgroundInactive = makeColor(0xff, 0xff, 0xff);
  Color titleInk = makeColor(0x1a, 0x1a, 0x1a);
  Color titleInkInactive = makeColor(0x99, 0x99, 0x99);
  Color buttonHover = makeColor(0xe8, 0xe8, 0xe8);
  Color buttonDown = makeColor(0xd8, 0xd8, 0xd8);
  Color closeHover = makeColor(0xe8, 0x11, 0x23);
  Color closeDown = makeColor(0xc5, 0x0e, 0x1d);
  Color closeInk = makeColor(0xff, 0xff, 0xff);
  Color bottomLine = makeColor(0xe5, 0xe5, 0xe5);

  // 是否在标题栏底部画一条分隔线。Windows 画，macOS 不画。
  bool drawBottomLine = true;

  int effectiveVisualHeight() const { return visualHeight > 0 ? visualHeight : bandHeight; }
};

// 当前平台的标题栏外观。默认实现返回 Windows 风格，供未实现平台的测试使用。
TitleBarAppearance titleBarAppearance();

}  // namespace pyui
