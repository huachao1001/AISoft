// 矢量图标：用画笔基础图元手绘
//
// 与字体字形图标并存。为什么不全用手绘：字形图标一行码点就能覆盖几十个
// 符号，粗细与墨迹框由字体自己保证，维护成本最低。手绘只在两种情况下值得：
//   1. 字形画不对语义——旋转画成圆环箭头，会被读成撤销/重做；
//   2. 同一排要统一观感，而字形来自不同字重、笔画粗细不一致。
//
// 几何全部写在 24 单位的作图网格上，按目标矩形等比缩放，任何尺寸比例都不变。
// 坐标用浮点：整数网格的线正好压在像素格上，1px 描边会被劈到相邻两行、各占
// 一半覆盖率，直边淡成浅灰、斜边出台阶。
#pragma once

#include "AiUI/icon.h"
#include "AiUI/types.h"

namespace AiUI {

class Painter;

// 用画笔手绘图标。返回 false 表示这个 id 没有手绘版本，调用方退回字形。
bool paintVectorIcon(Painter& painter, IconId id, const Rect& rc, Color color);

}  // namespace AiUI
