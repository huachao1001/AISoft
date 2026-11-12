// 界面配色与尺寸（96 DPI 设计值）
//
// 配色有两层：
//   1. Theme —— 运行时调色板。控件绘制时向所在视图树取当前主题，
//      因此换主题只需改一棵树的 theme，不必重建控件。
//   2. kColor* —— 历史常量，等价于浅色主题的取值。新代码请用 theme()。
//
// 全部用 AiUI::Color 表达，不依赖平台的颜色宏。
#pragma once

#include "AiUI/types.h"

namespace AiUI {

// 一棵视图树使用的调色板。字段按用途分组，语义与 kColor* 常量一一对应。
struct Theme {
  // ── 底色 ──
  Color body = makeColor(0xf3, 0xf4, 0xf6);
  Color tool = makeColor(0xf7, 0xf8, 0xfa);
  Color pane = makeColor(0xff, 0xff, 0xff);

  // ── 分隔线 ──
  Color line = makeColor(0xdd, 0xdd, 0xdd);
  Color lineSoft = makeColor(0xf3, 0xf4, 0xf6);
  Color lineHead = makeColor(0xe5, 0xe7, 0xeb);
  Color headerBg = makeColor(0xf9, 0xfa, 0xfb);

  // ── 文字 ──
  Color text = makeColor(0x33, 0x33, 0x33);
  Color textHead = makeColor(0x55, 0x55, 0x55);
  Color textMuted = makeColor(0x66, 0x66, 0x66);
  Color textFaint = makeColor(0xaa, 0xaa, 0xaa);
  Color ink = makeColor(0x44, 0x44, 0x44);

  // ── 链接 / 选中 / 悬停 ──
  Color link = makeColor(0x25, 0x63, 0xeb);
  Color linkHover = makeColor(0xe8, 0xf0, 0xfe);
  Color currentBg = makeColor(0xef, 0xf6, 0xff);
  Color selBg = makeColor(0xdb, 0xea, 0xfe);
  Color rowHover = makeColor(0xf7, 0xf8, 0xfa);
  Color hover = makeColor(0xf3, 0xf4, 0xf6);
  Color activeInk = makeColor(0x1d, 0x4e, 0xd8);

  // ── 按钮 ──
  Color btnHoverBg = makeColor(0xf0, 0xf4, 0xf9);
  Color btnDownBg = makeColor(0xe4, 0xeb, 0xf3);
  Color btnDisBg = makeColor(0xf8, 0xf8, 0xf8);

  // ── 状态栏标签 ──
  Color tagBg = makeColor(0xcf, 0xe2, 0xff);
  Color tagInk = makeColor(0x08, 0x42, 0x98);

  // ── 对话框底色 ──
  Color dialogBg = makeColor(240, 247, 252);

  // ── 输入类控件 ──
  Color inputBg = makeColor(0xff, 0xff, 0xff);
  Color inputFocus = makeColor(0x60, 0xa5, 0xfa);
  // 控件默认描边：输入框、下拉框、单选/复选、分组框共用同一支。
  // 各控件仍可单独覆盖，但默认外观要对齐，否则同一排控件会深浅不一。
  Color controlBorder = makeColor(0xd0, 0xd5, 0xdd);

  // ── 标题栏 ──
  Color titleBg = makeColor(0xff, 0xff, 0xff);
  Color titleBgInactive = makeColor(0xff, 0xff, 0xff);
  Color titleText = makeColor(0x1a, 0x1a, 0x1a);
  Color titleTextInactive = makeColor(0x99, 0x99, 0x99);
  Color titleButtonHover = makeColor(0xe8, 0xe8, 0xe8);
  Color titleButtonDown = makeColor(0xd8, 0xd8, 0xd8);
  Color closeHover = makeColor(0xe8, 0x11, 0x23);
  Color closeDown = makeColor(0xc5, 0x0e, 0x1d);
  Color closeInk = makeColor(0xff, 0xff, 0xff);
  Color titleLine = makeColor(0xe5, 0xe5, 0xe5);

  // ── 面包屑 ──
  Color separator = makeColor(0x9c, 0xa3, 0xaf);
  Color currentInk = makeColor(0x11, 0x11, 0x11);

  // ── 图标 ──
  Color iconMuted = makeColor(0x6b, 0x72, 0x80);
};

// 浅色主题即默认外观
inline constexpr Theme kLightTheme = Theme{};

inline constexpr Theme kDarkTheme = [] {
  Theme t;
  t.body = makeColor(0x1e, 0x1e, 0x1e);
  t.tool = makeColor(0x25, 0x25, 0x26);
  t.pane = makeColor(0x25, 0x25, 0x26);

  t.line = makeColor(0x3c, 0x3c, 0x3c);
  t.lineSoft = makeColor(0x2d, 0x2d, 0x2d);
  t.lineHead = makeColor(0x3c, 0x3c, 0x3c);
  t.headerBg = makeColor(0x2d, 0x2d, 0x2d);

  t.text = makeColor(0xd4, 0xd4, 0xd4);
  t.textHead = makeColor(0xb0, 0xb0, 0xb0);
  t.textMuted = makeColor(0x9a, 0x9a, 0x9a);
  t.textFaint = makeColor(0x6a, 0x6a, 0x6a);
  t.ink = makeColor(0xe0, 0xe0, 0xe0);

  t.link = makeColor(0x4a, 0x9e, 0xff);
  t.linkHover = makeColor(0x2a, 0x3f, 0x5f);
  t.currentBg = makeColor(0x2a, 0x3f, 0x5f);
  t.selBg = makeColor(0x2a, 0x4a, 0x6f);
  t.rowHover = makeColor(0x2a, 0x2d, 0x2e);
  t.hover = makeColor(0x2a, 0x2d, 0x2e);
  t.activeInk = makeColor(0x7a, 0xb8, 0xff);

  t.btnHoverBg = makeColor(0x33, 0x38, 0x3d);
  t.btnDownBg = makeColor(0x3f, 0x46, 0x50);
  t.btnDisBg = makeColor(0x2a, 0x2a, 0x2a);

  t.tagBg = makeColor(0x1e, 0x3a, 0x5f);
  t.tagInk = makeColor(0x9c, 0xc7, 0xff);

  t.dialogBg = makeColor(0x25, 0x25, 0x26);

  t.inputBg = makeColor(0x1e, 0x1e, 0x1e);
  t.inputFocus = makeColor(0x4a, 0x9e, 0xff);
  t.controlBorder = makeColor(0x4a, 0x4d, 0x52);

  t.titleBg = makeColor(0x20, 0x20, 0x20);
  t.titleBgInactive = makeColor(0x20, 0x20, 0x20);
  t.titleText = makeColor(0xd4, 0xd4, 0xd4);
  t.titleTextInactive = makeColor(0x80, 0x80, 0x80);
  t.titleButtonHover = makeColor(0x33, 0x33, 0x33);
  t.titleButtonDown = makeColor(0x40, 0x40, 0x40);
  t.closeHover = makeColor(0xe8, 0x11, 0x23);
  t.closeDown = makeColor(0xc5, 0x0e, 0x1d);
  t.closeInk = makeColor(0xff, 0xff, 0xff);
  t.titleLine = makeColor(0x3c, 0x3c, 0x3c);

  t.separator = makeColor(0x6a, 0x6a, 0x6a);
  t.currentInk = makeColor(0xe8, 0xe8, 0xe8);

  t.iconMuted = makeColor(0x8a, 0x91, 0x9e);
  return t;
}();

// 未挂到视图树时（例如布局阶段还没建窗）用的兜底色
inline const Theme& defaultTheme() { return kLightTheme; }

// ── 历史常量：等价于浅色主题取值 ──
// 控件绘制请改用 View::theme()，这些只保留给仍在写死颜色的调用方。
#define AIUI_LEGACY_COLOR(name, field) constexpr Color name = kLightTheme.field
AIUI_LEGACY_COLOR(kColorBody, body);
AIUI_LEGACY_COLOR(kColorTool, tool);
AIUI_LEGACY_COLOR(kColorPane, pane);
AIUI_LEGACY_COLOR(kColorLine, line);
AIUI_LEGACY_COLOR(kColorLineSoft, lineSoft);
AIUI_LEGACY_COLOR(kColorLineHead, lineHead);
AIUI_LEGACY_COLOR(kColorHeaderBg, headerBg);
AIUI_LEGACY_COLOR(kColorText, text);
AIUI_LEGACY_COLOR(kColorTextHead, textHead);
AIUI_LEGACY_COLOR(kColorTextMuted, textMuted);
AIUI_LEGACY_COLOR(kColorTextFaint, textFaint);
AIUI_LEGACY_COLOR(kColorInk, ink);
AIUI_LEGACY_COLOR(kColorLink, link);
AIUI_LEGACY_COLOR(kColorLinkHover, linkHover);
AIUI_LEGACY_COLOR(kColorCurrentBg, currentBg);
AIUI_LEGACY_COLOR(kColorSelBg, selBg);
AIUI_LEGACY_COLOR(kColorRowHover, rowHover);
AIUI_LEGACY_COLOR(kColorHover, hover);
AIUI_LEGACY_COLOR(kColorActiveInk, activeInk);
AIUI_LEGACY_COLOR(kColorBtnHoverBg, btnHoverBg);
AIUI_LEGACY_COLOR(kColorBtnDownBg, btnDownBg);
AIUI_LEGACY_COLOR(kColorBtnDisBg, btnDisBg);
AIUI_LEGACY_COLOR(kColorTagBg, tagBg);
AIUI_LEGACY_COLOR(kColorTagInk, tagInk);
AIUI_LEGACY_COLOR(kColorDialogBg, dialogBg);
AIUI_LEGACY_COLOR(kColorInputBg, inputBg);
AIUI_LEGACY_COLOR(kColorInputFocus, inputFocus);
#undef AIUI_LEGACY_COLOR

// 依底色明暗派生描边色：浅底叠淡墨、深底叠淡白。
// 控件不该写死描边色——换主题、换宿主底色时，固定灰要么糊进背景、
// 要么在深底上刺眼。percent 是在底色上叠加的墨色浓度。
constexpr Color edgeOn(Color background, int percent = 18) {
  const int luma = (colorRed(background) * 299 + colorGreen(background) * 587 +
                    colorBlue(background) * 114) /
                   1000;
  const Color ink = luma < 128 ? makeColor(0xff, 0xff, 0xff) : makeColor(0x00, 0x00, 0x00);
  return blendColor(background, ink, static_cast<float>(percent) / 100.0f);
}

// ── 布局尺寸 ──
constexpr int kSizeToolH = 52;
constexpr int kSizeCrumbH = 48;
constexpr int kSizeStatusH = 36;
constexpr int kSizeSidebarW = 230;
constexpr int kSizeRowH = 44;
constexpr int kSizeHeaderH = 40;

}  // namespace AiUI
