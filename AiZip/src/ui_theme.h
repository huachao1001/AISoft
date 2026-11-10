// 界面配色与尺寸
#pragma once

#include <windows.h>

namespace ui {

// 底色
constexpr COLORREF kBodyBg = RGB(0xf3, 0xf4, 0xf6);
constexpr COLORREF kToolBg = RGB(0xf7, 0xf8, 0xfa);
constexpr COLORREF kPaneBg = RGB(0xff, 0xff, 0xff);

// 分隔线
constexpr COLORREF kLine = RGB(0xdd, 0xdd, 0xdd);
constexpr COLORREF kLineSoft = RGB(0xf3, 0xf4, 0xf6);
constexpr COLORREF kLineHead = RGB(0xe5, 0xe7, 0xeb);
constexpr COLORREF kThBg = RGB(0xf9, 0xfa, 0xfb);

// 文字
constexpr COLORREF kText = RGB(0x33, 0x33, 0x33);
constexpr COLORREF kTextHead = RGB(0x55, 0x55, 0x55);
constexpr COLORREF kTextMuted = RGB(0x66, 0x66, 0x66);
constexpr COLORREF kTextFaint = RGB(0xaa, 0xaa, 0xaa);
constexpr COLORREF kInk = RGB(0x44, 0x44, 0x44);

// 面包屑
constexpr COLORREF kLink = RGB(0x25, 0x63, 0xeb);
constexpr COLORREF kLinkHover = RGB(0xe8, 0xf0, 0xfe);
constexpr COLORREF kCurrentBg = RGB(0xef, 0xf6, 0xff);

// 选中 / 悬停
constexpr COLORREF kSelBg = RGB(0xdb, 0xea, 0xfe);
constexpr COLORREF kRowHover = RGB(0xf7, 0xf8, 0xfa);
constexpr COLORREF kHoverBg = RGB(0xf3, 0xf4, 0xf6);
constexpr COLORREF kActiveInk = RGB(0x1d, 0x4e, 0xd8);

// 按钮
constexpr COLORREF kBtnBorder = RGB(0xd1, 0xd5, 0xdb);
constexpr COLORREF kBtnHoverBg = RGB(0xf0, 0xf4, 0xf9);
constexpr COLORREF kBtnHoverBorder = RGB(0xb9, 0xc8, 0xdd);
constexpr COLORREF kBtnDownBg = RGB(0xe4, 0xeb, 0xf3);
constexpr COLORREF kBtnDisBg = RGB(0xf8, 0xf8, 0xf8);
constexpr COLORREF kBtnDisBorder = RGB(0xee, 0xee, 0xee);

// 状态栏标签
constexpr COLORREF kTagBg = RGB(0xcf, 0xe2, 0xff);
constexpr COLORREF kTagInk = RGB(0x08, 0x42, 0x98);

// 对话框底色：控件（标签/分组框/单选/复选）也铺这个色，避免白块
constexpr COLORREF kDialogBg = RGB(240, 247, 252);

// 布局尺寸（96 DPI 基准）
constexpr int kToolH = 52;
constexpr int kCrumbH = 48;
constexpr int kStatusH = 36;
constexpr int kSidebarW = 230;
constexpr int kRowH = 44;
constexpr int kHeadH = 40;

// 输入框
constexpr COLORREF kInputBorder = RGB(0xd1, 0xd5, 0xdb);
constexpr COLORREF kInputFocusBorder = RGB(0x60, 0xa5, 0xfa);
constexpr COLORREF kInputBg = RGB(0xff, 0xff, 0xff);

}  // namespace ui
