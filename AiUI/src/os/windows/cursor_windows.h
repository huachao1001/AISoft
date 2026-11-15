// 自绘光标：系统没有「手掌抓取」这一对，只能自己画
//
// 为什么要两只：kGrab 是悬停时张开的手，kGrabbing 是按下去之后收拢的手。
// 二者必须一眼能分辨——否则用户按了半天看不出到底有没有抓住画布。
//
// 几何写在 32×32 的设计空间里，按 SM_CXCURSOR 等比放大；用 GDI+ 在 4 倍
// 超采样下画好再缩小，边缘才有抗锯齿。形状由「描边色铺满 + 实色盖回」两步
// 组成：白色盖住描边的内半边，只留外半边，指与掌重叠处不会残留内部线条，
// 比求并集简单且效果一致。
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
