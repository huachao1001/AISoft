// 自绘光标：系统没有「手掌抓取」这一对，只能自己画
//
// 为什么要两只：kGrab 是张开的手（提示「可以抓住拖」），kGrabbing 是按下去
// 之后收拢的手。二者必须能分辨——否则用户看不出这次按下有没有抓住画布。
// 控件可以只在拖拽时用 kGrabbing，但形状本身仍要成对提供。
//
// 几何写在 32×32 的设计空间里，按 SM_CXCURSOR 等比放大；用 GDI+ 在 4 倍
// 超采样下画好再缩小，边缘才有抗锯齿。形状由「填充 + 区域外描边」两步组成：
// 粗笔只有外半边可见，等于只给剪影描边，内部不会残留接缝。
#pragma once

#include <windows.h>

namespace AiUI {
namespace os {
namespace windows {

// 取/建指定形状的手掌光标。句柄由本模块持有，退出时统一释放，调用方不要删。
// shape 只接受 kGrab / kGrabbing；其它值返回 nullptr。
HICON handCursor(bool grabbing);
// 释放已建的光标（退出前调用一次）
void releaseCursorCache();

}  // namespace windows
}  // namespace os
}  // namespace AiUI
