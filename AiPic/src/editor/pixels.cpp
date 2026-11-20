// 像素运算实现
//
// alpha 处理是这里最容易出错的地方：非预乘 alpha 直接线性插值，半透明像素
// 与全透明像素混出来的颜色会偏暗（透明处 RGB 通常是 0，被平均进去就成了灰边）。
// 所以重采样一律先转预乘、插值后再除回去。
#include "editor/pixels.h"

#include <algorithm>
#include <cmath>

namespace editor {
namespace {

inline int alphaOf(uint32_t c) { return static_cast<int>((c >> 24) & 0xff); }
inline int redOf(uint32_t c) { return static_cast<int>((c >> 16) & 0xff); }
inline int greenOf(uint32_t c) { return static_cast<int>((c >> 8) & 0xff); }
inline int blueOf(uint32_t c) { return static_cast<int>(c & 0xff); }

inline uint32_t packArgb(int a, int r, int g, int b) {
  return (static_cast<uint32_t>(std::clamp(a, 0, 255)) << 24) |
         (static_cast<uint32_t>(std::clamp(r, 0, 255)) << 16) |
         (static_cast<uint32_t>(std::clamp(g, 0, 255)) << 8) |
         static_cast<uint32_t>(std::clamp(b, 0, 255));
}

// 预乘：把 alpha 乘进 RGB。透明像素的 RGB 于是也是 0，
// 加权平均时不会把「看不见的杂色」带进结果。
struct Premul {
  float a = 0.0f;
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
};

inline Premul toPremul(uint32_t c) {
  const float a = static_cast<float>(alphaOf(c)) / 255.0f;
  return Premul{a, static_cast<float>(redOf(c)) / 255.0f * a,
                static_cast<float>(greenOf(c)) / 255.0f * a,
                static_cast<float>(blueOf(c)) / 255.0f * a};
}

inline uint32_t fromPremul(const Premul& p) {
  if (p.a <= 0.0001f) return 0;
  const float inv = 1.0f / p.a;
  return packArgb(static_cast<int>(std::lround(p.a * 255.0f)),
                  static_cast<int>(std::lround(p.r * inv * 255.0f)),
                  static_cast<int>(std::lround(p.g * inv * 255.0f)),
                  static_cast<int>(std::lround(p.b * inv * 255.0f)));
}

inline uint32_t pixelAt(const AiUI::RgbaImage& img, int x, int y) {
  x = std::clamp(x, 0, img.width - 1);
  y = std::clamp(y, 0, img.height - 1);
  return img.pixels[static_cast<size_t>(y) * img.width + x];
}

// 区域平均只统计真正被覆盖的源像素范围：按目标像素反推源区间，
// 这样缩小时每个源像素都参与恰好一次，不会因为取整漏掉整行整列。
AiUI::RgbaImage resampleBox(const AiUI::RgbaImage& src, int w, int h) {
  AiUI::RgbaImage dst;
  dst.width = w;
  dst.height = h;
  dst.pixels.resize(static_cast<size_t>(w) * h);

  const double sx = static_cast<double>(src.width) / w;
  const double sy = static_cast<double>(src.height) / h;

  for (int y = 0; y < h; ++y) {
    const int y0 = static_cast<int>(std::floor(y * sy));
    const int y1 = (std::max)(y0 + 1, static_cast<int>(std::ceil((y + 1) * sy)));
    for (int x = 0; x < w; ++x) {
      const int x0 = static_cast<int>(std::floor(x * sx));
      const int x1 = (std::max)(x0 + 1, static_cast<int>(std::ceil((x + 1) * sx)));

      Premul sum;
      int count = 0;
      for (int syi = y0; syi < y1 && syi < src.height; ++syi) {
        for (int sxi = x0; sxi < x1 && sxi < src.width; ++sxi) {
          const Premul p = toPremul(src.pixels[static_cast<size_t>(syi) * src.width + sxi]);
          sum.a += p.a;
          sum.r += p.r;
          sum.g += p.g;
          sum.b += p.b;
          ++count;
        }
      }
      if (count == 0) {
        dst.pixels[static_cast<size_t>(y) * w + x] = 0;
        continue;
      }
      const float inv = 1.0f / static_cast<float>(count);
      dst.pixels[static_cast<size_t>(y) * w + x] =
          fromPremul(Premul{sum.a * inv, sum.r * inv, sum.g * inv, sum.b * inv});
    }
  }
  return dst;
}

AiUI::RgbaImage resampleBilinear(const AiUI::RgbaImage& src, int w, int h) {
  AiUI::RgbaImage dst;
  dst.width = w;
  dst.height = h;
  dst.pixels.resize(static_cast<size_t>(w) * h);

  // 用「像素中心对齐」的映射：直接把 0..w-1 映射到 0..srcW-1 会让图形整体
  // 偏半个像素，细线放大后左右不对称
  const double sx = static_cast<double>(src.width) / w;
  const double sy = static_cast<double>(src.height) / h;

  for (int y = 0; y < h; ++y) {
    const double fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, static_cast<double>(src.height - 1));
    const int y0 = static_cast<int>(std::floor(fy));
    const int y1 = (std::min)(y0 + 1, src.height - 1);
    const float wy = static_cast<float>(fy - y0);

    for (int x = 0; x < w; ++x) {
      const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, static_cast<double>(src.width - 1));
      const int x0 = static_cast<int>(std::floor(fx));
      const int x1 = (std::min)(x0 + 1, src.width - 1);
      const float wx = static_cast<float>(fx - x0);

      const Premul p00 = toPremul(pixelAt(src, x0, y0));
      const Premul p10 = toPremul(pixelAt(src, x1, y0));
      const Premul p01 = toPremul(pixelAt(src, x0, y1));
      const Premul p11 = toPremul(pixelAt(src, x1, y1));

      auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
      Premul top;
      top.a = lerp(p00.a, p10.a, wx);
      top.r = lerp(p00.r, p10.r, wx);
      top.g = lerp(p00.g, p10.g, wx);
      top.b = lerp(p00.b, p10.b, wx);
      Premul bottom;
      bottom.a = lerp(p01.a, p11.a, wx);
      bottom.r = lerp(p01.r, p11.r, wx);
      bottom.g = lerp(p01.g, p11.g, wx);
      bottom.b = lerp(p01.b, p11.b, wx);

      Premul out;
      out.a = lerp(top.a, bottom.a, wy);
      out.r = lerp(top.r, bottom.r, wy);
      out.g = lerp(top.g, bottom.g, wy);
      out.b = lerp(top.b, bottom.b, wy);
      dst.pixels[static_cast<size_t>(y) * w + x] = fromPremul(out);
    }
  }
  return dst;
}

AiUI::RgbaImage resampleNearest(const AiUI::RgbaImage& src, int w, int h) {
  AiUI::RgbaImage dst;
  dst.width = w;
  dst.height = h;
  dst.pixels.resize(static_cast<size_t>(w) * h);

  for (int y = 0; y < h; ++y) {
    const int syi = std::clamp(static_cast<int>(static_cast<double>(y) * src.height / h), 0,
                               src.height - 1);
    for (int x = 0; x < w; ++x) {
      const int sxi = std::clamp(static_cast<int>(static_cast<double>(x) * src.width / w), 0,
                                 src.width - 1);
      dst.pixels[static_cast<size_t>(y) * w + x] =
          src.pixels[static_cast<size_t>(syi) * src.width + sxi];
    }
  }
  return dst;
}

}  // namespace

AiUI::Size fitSize(AiUI::Size src, AiUI::Size box, bool allowUpscale) {
  if (src.width <= 0 || src.height <= 0 || box.width <= 0 || box.height <= 0) {
    return AiUI::Size{1, 1};
  }
  double scale = (std::min)(static_cast<double>(box.width) / src.width,
                            static_cast<double>(box.height) / src.height);
  if (!allowUpscale) scale = (std::min)(scale, 1.0);
  const int w = (std::max)(1, static_cast<int>(std::lround(src.width * scale)));
  const int h = (std::max)(1, static_cast<int>(std::lround(src.height * scale)));
  return AiUI::Size{w, h};
}

AiUI::RgbaImage resample(const AiUI::RgbaImage& src, int width, int height,
                         Resample method) {
  if (src.isEmpty() || width <= 0 || height <= 0) return AiUI::RgbaImage{};
  if (width == src.width && height == src.height) return src;

  // 缩小时即使调用方要双线性，也换成区域平均：双线性会跳过源像素，
  // 高频细节会变成摩尔纹；放大时区域平均只会更糊，所以反过来替成双线性
  const bool shrinking = width < src.width || height < src.height;
  Resample use = method;
  if (method == Resample::kBilinear && shrinking) use = Resample::kBox;
  if (method == Resample::kBox && !shrinking) use = Resample::kBilinear;

  switch (use) {
    case Resample::kNearest: return resampleNearest(src, width, height);
    case Resample::kBox: return resampleBox(src, width, height);
    default: return resampleBilinear(src, width, height);
  }
}

AiUI::RgbaImage crop(const AiUI::RgbaImage& src, const AiUI::Rect& region) {
  if (src.isEmpty()) return AiUI::RgbaImage{};

  const AiUI::Rect clipped =
      region.intersect(AiUI::Rect{0, 0, src.width, src.height});
  if (clipped.isEmpty()) return AiUI::RgbaImage{};

  AiUI::RgbaImage dst;
  dst.width = clipped.width();
  dst.height = clipped.height();
  dst.pixels.resize(static_cast<size_t>(dst.width) * dst.height);

  for (int y = 0; y < dst.height; ++y) {
    const size_t srcRow = static_cast<size_t>(clipped.top + y) * src.width + clipped.left;
    std::copy_n(src.pixels.begin() + static_cast<std::ptrdiff_t>(srcRow), dst.width,
                dst.pixels.begin() + static_cast<std::ptrdiff_t>(y) * dst.width);
  }
  return dst;
}

AiUI::RgbaImage rotate90(const AiUI::RgbaImage& src, int quarterTurns) {
  const int turns = ((quarterTurns % 4) + 4) % 4;
  if (turns == 0 || src.isEmpty()) return src;

  const bool swap = (turns % 2) == 1;
  AiUI::RgbaImage dst;
  dst.width = swap ? src.height : src.width;
  dst.height = swap ? src.width : src.height;
  dst.pixels.resize(static_cast<size_t>(dst.width) * dst.height);

  for (int y = 0; y < dst.height; ++y) {
    for (int x = 0; x < dst.width; ++x) {
      int sx = 0;
      int sy = 0;
      switch (turns) {
        case 1: sx = y; sy = src.height - 1 - x; break;
        case 2: sx = src.width - 1 - x; sy = src.height - 1 - y; break;
        default: sx = src.width - 1 - y; sy = x; break;
      }
      dst.pixels[static_cast<size_t>(y) * dst.width + x] =
          src.pixels[static_cast<size_t>(sy) * src.width + sx];
    }
  }
  return dst;
}

AiUI::RgbaImage flip(const AiUI::RgbaImage& src, bool horizontal, bool vertical) {
  if (src.isEmpty()) return src;
  AiUI::RgbaImage dst = src;
  for (int y = 0; y < src.height; ++y) {
    const int sy = vertical ? src.height - 1 - y : y;
    for (int x = 0; x < src.width; ++x) {
      const int sx = horizontal ? src.width - 1 - x : x;
      dst.pixels[static_cast<size_t>(y) * src.width + x] =
          src.pixels[static_cast<size_t>(sy) * src.width + sx];
    }
  }
  return dst;
}

}  // namespace editor
