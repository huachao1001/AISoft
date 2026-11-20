// 像素运算：重采样、裁剪、旋转
//
// 全部是纯函数，不碰平台后端。编辑器所有「改像素」的动作都经过这里，
// 这样测试可以直接喂 RgbaImage 断言结果，不需要开窗口。
#pragma once

#include "AiUI/icon.h"

#include <vector>

namespace editor {

// 缩放算法。降采样时最近邻会出现明显摩尔纹，所以默认走区域平均；
// 放大时区域平均会糊，改用双线性。
enum class Resample {
  kNearest,   // 像素画、图标放大：要保住硬边
  kBilinear,  // 一般放大
  kBox,       // 降采样：每块源像素等权平均，不会跳过任何像素
};

// 等比缩放到目标框内（不放大，除非 allowUpscale）。
// 返回的尺寸至少为 1×1。
AiUI::Size fitSize(AiUI::Size src, AiUI::Size box, bool allowUpscale);

// 按指定宽高重采样。宽高任意（不保比例），由调用方先用 fitSize 算好。
AiUI::RgbaImage resample(const AiUI::RgbaImage& src, int width, int height,
                         Resample method);

// 裁剪：region 是原图像素坐标，超出部分自动夹到图像边界内。
AiUI::RgbaImage crop(const AiUI::RgbaImage& src, const AiUI::Rect& region);

// 顺时针 90° 整数倍旋转。
AiUI::RgbaImage rotate90(const AiUI::RgbaImage& src, int quarterTurns);

// 水平/垂直翻转。
AiUI::RgbaImage flip(const AiUI::RgbaImage& src, bool horizontal, bool vertical);

}  // namespace editor
