// 界面配色与尺寸（96 DPI 设计值）
//
// 全部用 AiUI::Color 表达，不依赖平台的颜色宏。
#pragma once

#include "AiUI/types.h"

namespace AiUI {

// ── 底色 ──
constexpr Color kColorBody = makeColor(0xf3, 0xf4, 0xf6);
constexpr Color kColorTool = makeColor(0xf7, 0xf8, 0xfa);
constexpr Color kColorPane = makeColor(0xff, 0xff, 0xff);

// ── 分隔线 ──
constexpr Color kColorLine = makeColor(0xdd, 0xdd, 0xdd);
constexpr Color kColorLineSoft = makeColor(0xf3, 0xf4, 0xf6);
constexpr Color kColorLineHead = makeColor(0xe5, 0xe7, 0xeb);
constexpr Color kColorHeaderBg = makeColor(0xf9, 0xfa, 0xfb);

// ── 文字 ──
constexpr Color kColorText = makeColor(0x33, 0x33, 0x33);
constexpr Color kColorTextHead = makeColor(0x55, 0x55, 0x55);
constexpr Color kColorTextMuted = makeColor(0x66, 0x66, 0x66);
constexpr Color kColorTextFaint = makeColor(0xaa, 0xaa, 0xaa);
constexpr Color kColorInk = makeColor(0x44, 0x44, 0x44);

// ── 面包屑 ──
constexpr Color kColorLink = makeColor(0x25, 0x63, 0xeb);
constexpr Color kColorLinkHover = makeColor(0xe8, 0xf0, 0xfe);
constexpr Color kColorCurrentBg = makeColor(0xef, 0xf6, 0xff);

// ── 选中 / 悬停 ──
constexpr Color kColorSelBg = makeColor(0xdb, 0xea, 0xfe);
constexpr Color kColorRowHover = makeColor(0xf7, 0xf8, 0xfa);
constexpr Color kColorHover = makeColor(0xf3, 0xf4, 0xf6);
constexpr Color kColorActiveInk = makeColor(0x1d, 0x4e, 0xd8);

// ── 按钮 ──
constexpr Color kColorBtnBorder = makeColor(0xd1, 0xd5, 0xdb);
constexpr Color kColorBtnHoverBg = makeColor(0xf0, 0xf4, 0xf9);
constexpr Color kColorBtnHoverBorder = makeColor(0xb9, 0xc8, 0xdd);
constexpr Color kColorBtnDownBg = makeColor(0xe4, 0xeb, 0xf3);
constexpr Color kColorBtnDisBg = makeColor(0xf8, 0xf8, 0xf8);
constexpr Color kColorBtnDisBorder = makeColor(0xee, 0xee, 0xee);

// ── 状态栏标签 ──
constexpr Color kColorTagBg = makeColor(0xcf, 0xe2, 0xff);
constexpr Color kColorTagInk = makeColor(0x08, 0x42, 0x98);

// ── 对话框底色 ──
constexpr Color kColorDialogBg = makeColor(240, 247, 252);

// ── 输入框 ──
constexpr Color kColorInputBorder = makeColor(0xd1, 0xd5, 0xdb);
constexpr Color kColorInputFocus = makeColor(0x60, 0xa5, 0xfa);
constexpr Color kColorInputBg = makeColor(0xff, 0xff, 0xff);

// ── 布局尺寸 ──
constexpr int kSizeToolH = 52;
constexpr int kSizeCrumbH = 48;
constexpr int kSizeStatusH = 36;
constexpr int kSizeSidebarW = 230;
constexpr int kSizeRowH = 44;
constexpr int kSizeHeaderH = 40;

}  // namespace AiUI
