// 像素运算测试
//
// 重采样、裁剪这些是编辑器的地基本身，出错不会崩、只会悄悄把图弄糊或偏色，
// 所以每一条都要有确定断言，不能靠肉眼看预览图。
#include "../src/editor/pixels.h"

#include "test_framework.h"

#include <cmath>

namespace {

using namespace aiui_test;
using namespace AiUI;

RgbaImage solid(int w, int h, uint32_t color) {
  RgbaImage img;
  img.width = w;
  img.height = h;
  img.pixels.assign(static_cast<size_t>(w) * h, color);
  return img;
}

uint32_t at(const RgbaImage& img, int x, int y) {
  return img.pixels[static_cast<size_t>(y) * img.width + x];
}

constexpr uint32_t kRed = 0xffff0000u;
constexpr uint32_t kBlue = 0xff0000ffu;
constexpr uint32_t kWhite = 0xffffffffu;

}  // namespace

// 适应尺寸：只缩小不放大；等比；最小 1 像素
AIUI_TEST(pixels_fit_size_keeps_ratio) {
  AIUI_CHECK(editor::fitSize(Size{200, 100}, Size{100, 100}, false).width == 100);
  AIUI_CHECK(editor::fitSize(Size{200, 100}, Size{100, 100}, false).height == 50);
  // 小图不放大
  const Size same = editor::fitSize(Size{50, 50}, Size{400, 400}, false);
  AIUI_CHECK(same.width == 50);
  AIUI_CHECK(same.height == 50);
  // 允许放大时铺满框
  const Size up = editor::fitSize(Size{50, 50}, Size{400, 400}, true);
  AIUI_CHECK(up.width == 400);
  // 极端窄的图不能算出 0
  AIUI_CHECK(editor::fitSize(Size{1000, 1}, Size{10, 10}, false).height >= 1);
}

// 放大 2 倍：纯色图必须仍是同一个颜色，不能因为插值引入偏差
AIUI_TEST(pixels_resample_solid_keeps_color) {
  const RgbaImage src = solid(4, 4, kRed);
  const RgbaImage out = editor::resample(src, 8, 8, editor::Resample::kBilinear);
  AIUI_CHECK(out.width == 8);
  AIUI_CHECK(out.height == 8);
  for (uint32_t px : out.pixels) AIUI_CHECK(px == kRed);
}

// 缩小 2 倍：纯色图同样保持原色
AIUI_TEST(pixels_resample_shrink_keeps_color) {
  const RgbaImage src = solid(8, 8, kBlue);
  const RgbaImage out = editor::resample(src, 4, 4, editor::Resample::kBox);
  AIUI_CHECK(out.width == 4);
  for (uint32_t px : out.pixels) AIUI_CHECK(px == kBlue);
}

// 区域平均取的是「被覆盖的源像素」，不是随便采一个点：
// 左半红右半蓝缩成 1 列，结果必须是紫色而不是纯红或纯蓝
AIUI_TEST(pixels_box_average_mixes_halves) {
  RgbaImage src;
  src.width = 4;
  src.height = 1;
  src.pixels = {kRed, kRed, kBlue, kBlue};

  const RgbaImage out = editor::resample(src, 1, 1, editor::Resample::kBox);
  AIUI_CHECK(out.width == 1);
  AIUI_CHECK(out.height == 1);
  const uint32_t px = at(out, 0, 0);
  const int r = static_cast<int>((px >> 16) & 0xff);
  const int b = static_cast<int>(px & 0xff);
  // 各占一半：红蓝都应接近 128
  AIUI_CHECK(std::abs(r - 128) <= 8);
  AIUI_CHECK(std::abs(b - 128) <= 8);
}

// 最近邻：放大 2 倍后每个源像素变成整齐的 2×2 块，边缘不许被糊掉。
// 像素画和图标拉伸靠的就是这个性质。
AIUI_TEST(pixels_nearest_preserves_hard_edges) {
  RgbaImage src;
  src.width = 2;
  src.height = 1;
  src.pixels = {kRed, kBlue};

  const RgbaImage out = editor::resample(src, 4, 1, editor::Resample::kNearest);
  AIUI_CHECK(at(out, 0, 0) == kRed);
  AIUI_CHECK(at(out, 1, 0) == kRed);
  AIUI_CHECK(at(out, 2, 0) == kBlue);
  AIUI_CHECK(at(out, 3, 0) == kBlue);
}

// 半透明与全透明混在一起时不能发黑：
// 透明像素的 RGB 是 0，如果直接平均会把红色拉暗，正确做法要按 alpha 加权
AIUI_TEST(pixels_alpha_does_not_darken_edges) {
  RgbaImage src;
  src.width = 2;
  src.height = 1;
  // 一个不透明红，一个全透明（RGB 为 0）
  src.pixels = {kRed, 0x00000000u};

  const RgbaImage out = editor::resample(src, 1, 1, editor::Resample::kBox);
  const uint32_t px = at(out, 0, 0);
  const int r = static_cast<int>((px >> 16) & 0xff);
  const int a = static_cast<int>((px >> 24) & 0xff);
  // 半透明、颜色仍是纯红：RGB 不该被透明那半拉黑
  AIUI_CHECK(std::abs(a - 128) <= 8);
  AIUI_CHECK(r >= 240);
}

// 裁剪：正常区域、越界区域、完全在外
AIUI_TEST(pixels_crop_basic_and_clamped) {
  RgbaImage src = solid(10, 10, kRed);
  src.pixels[0] = kBlue;  // 左上角做个标记

  const RgbaImage out = editor::crop(src, Rect{0, 0, 4, 4});
  AIUI_CHECK(out.width == 4);
  AIUI_CHECK(out.height == 4);
  AIUI_CHECK(at(out, 0, 0) == kBlue);

  // 越界：夹到边界内，不能崩也不能返回空
  const RgbaImage clamped = editor::crop(src, Rect{-5, -5, 5, 5});
  AIUI_CHECK(clamped.width == 5);
  AIUI_CHECK(clamped.height == 5);

  // 完全在外：返回空图
  AIUI_CHECK(editor::crop(src, Rect{100, 100, 110, 110}).isEmpty());
}

// 旋转：顺时针 1 格后，原来的左上角跑到右上角
AIUI_TEST(pixels_rotate90_moves_corner) {
  RgbaImage src = solid(3, 2, kRed);
  src.pixels[0] = kBlue;  // (0,0) 是蓝

  const RgbaImage out = editor::rotate90(src, 1);
  AIUI_CHECK(out.width == 2);
  AIUI_CHECK(out.height == 3);
  // 顺时针：原来的左上 (0,0) 落到 (width-1, 0)
  AIUI_CHECK(at(out, out.width - 1, 0) == kBlue);

  // 转 4 次回到原样
  RgbaImage back = src;
  for (int i = 0; i < 4; ++i) back = editor::rotate90(back, 1);
  AIUI_CHECK(back.width == src.width);
  AIUI_CHECK(at(back, 0, 0) == kBlue);
}

// 翻转：水平翻转后左上角跑到右上角
AIUI_TEST(pixels_flip_moves_corner) {
  RgbaImage src = solid(4, 3, kRed);
  src.pixels[0] = kBlue;

  const RgbaImage h = editor::flip(src, true, false);
  AIUI_CHECK(at(h, h.width - 1, 0) == kBlue);
  AIUI_CHECK(at(h, 0, 0) == kRed);

  const RgbaImage v = editor::flip(src, false, true);
  AIUI_CHECK(at(v, 0, v.height - 1) == kBlue);

  // 翻两次回到原样
  const RgbaImage twice = editor::flip(h, true, false);
  AIUI_CHECK(at(twice, 0, 0) == kBlue);
}

// 尺寸相同的重采样直接返回原图，不做无谓的重算：
// 预览每帧都会调它，否则拖动滑块时会一直全图重采样
AIUI_TEST(pixels_resample_same_size_is_noop) {
  const RgbaImage src = solid(5, 5, kWhite);
  const RgbaImage out = editor::resample(src, 5, 5, editor::Resample::kBilinear);
  AIUI_CHECK(out.width == 5);
  AIUI_CHECK(at(out, 0, 0) == kWhite);
}
