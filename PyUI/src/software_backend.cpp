#include "pyui/software_backend.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pyui {
namespace {

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::uint8_t toByte(int v) {
  return static_cast<std::uint8_t>(clampInt(v, 0, 255));
}

// 连续坐标上的圆角矩形内部判定。
// 用「到内缩矩形的最近点距离」而不是逐角判断：半径大于半宽时
// （理论上已被夹紧）也不会算错，且只有一处分支。
bool insideRoundRect(float px, float py, const Rect& rc, float r) {
  if (px < static_cast<float>(rc.left) || px >= static_cast<float>(rc.right) ||
      py < static_cast<float>(rc.top) || py >= static_cast<float>(rc.bottom)) {
    return false;
  }
  if (r <= 0.0f) return true;
  const float cx = std::min(std::max(px, static_cast<float>(rc.left) + r),
                            static_cast<float>(rc.right) - r);
  const float cy = std::min(std::max(py, static_cast<float>(rc.top) + r),
                            static_cast<float>(rc.bottom) - r);
  const float dx = px - cx;
  const float dy = py - cy;
  return dx * dx + dy * dy <= r * r;
}

// 直通道（非预乘）alpha 合成。
//
// 表面初值是全 0（完全透明），所以「画一层半透明色」得到的是该色与其 alpha；
// 「再画一层」才会走真正的 over 合成。用非预乘而不是预乘，是因为
// Surface 的文档把像素定义为 0xAARRGGBB，读像素做断言时不会被 alpha 干扰。
void blendCovered(Surface& surface, int x, int y, Color color, std::uint8_t coverage) {
  if (x < 0 || y < 0 || x >= surface.width() || y >= surface.height()) return;
  const int sa = static_cast<int>(colorAlpha(color)) * static_cast<int>(coverage) / 255;
  if (sa <= 0) return;

  std::uint32_t* row = surface.row(y);
  if (row == nullptr) return;

  // 快速路径：完全不透明的覆盖直接写入，省掉每通道的除法
  if (sa >= 255) {
    row[x] = color | 0xFF000000u;
    return;
  }

  const std::uint32_t dst = row[x];
  const int da = static_cast<int>(colorAlpha(dst));
  const int outA = sa + da * (255 - sa) / 255;
  if (outA <= 0) return;

  auto mix = [&](int sc, int dc) {
    const int weighted = sc * sa + dc * da * (255 - sa) / 255;
    return toByte(weighted / outA);
  };

  row[x] = makeColor(mix(static_cast<int>(colorRed(color)), static_cast<int>(colorRed(dst))),
                     mix(static_cast<int>(colorGreen(color)), static_cast<int>(colorGreen(dst))),
                     mix(static_cast<int>(colorBlue(color)), static_cast<int>(colorBlue(dst))),
                     toByte(outA));
}

// 竖直线性插值。t 取 0..255，0 对应 top、255 对应 bottom。
Color lerpColor(Color top, Color bottom, int t) {
  auto channel = [t](std::uint8_t a, std::uint8_t b) {
    return toByte(static_cast<int>(a) + (static_cast<int>(b) - static_cast<int>(a)) * t / 255);
  };
  return makeColor(channel(colorRed(top), colorRed(bottom)),
                   channel(colorGreen(top), colorGreen(bottom)),
                   channel(colorBlue(top), colorBlue(bottom)),
                   channel(colorAlpha(top), colorAlpha(bottom)));
}

// 32 位整数版本的整数平方根，用于图标里的圆弧采样（不需要浮点，
// 也能避免不同 CRT 的浮点行为差异影响像素结果）。
int isqrt(int v) {
  if (v <= 0) return 0;
  int r = 0;
  int bit = 1 << 30;
  while (bit > v) bit >>= 2;
  while (bit != 0) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return r;
}

}  // namespace

// ── 构造 ──

SoftwareBackend::SoftwareBackend(Surface& target, Options options)
    : target_(target), options_(options) {
  if (options_.samplesPerAxis < 1) options_.samplesPerAxis = 1;
  state_.clip = Rect::fromSize(target_.width(), target_.height());
  state_.clipValid = true;
}

SoftwareBackend::~SoftwareBackend() = default;

Rect SoftwareBackend::toSurface(const Rect& rc) const {
  return rc.moved(state_.tx, state_.ty);
}

template <typename Fn>
void SoftwareBackend::forEachSpan(const Rect& rc, Fn&& fn) {
  Rect span = toSurface(rc);
  if (state_.clipValid) span = span.intersect(state_.clip);
  span = span.intersect(Rect::fromSize(target_.width(), target_.height()));
  if (span.isEmpty()) {
    ++culled_;
    return;
  }
  for (int y = span.top; y < span.bottom; ++y) fn(span.left, span.right, y);
}

// ── 覆盖率 ──

std::uint8_t SoftwareBackend::roundRectCoverage(int px, int py, const Rect& rc,
                                                float radius) const {
  if (rc.isEmpty()) return 0;

  // 快路径：完全落在「四角圆弧带」之外时覆盖率必然是 255。
  // 大面积底色（整个客户区）几乎全部走这条路，不必每像素采 16 次。
  if (radius > 0.0f) {
    const int r = static_cast<int>(std::ceil(radius));
    const Rect core = rc.inset(r, r, r, r);
    if (!core.isEmpty() && px >= core.left && px < core.right && py >= core.top &&
        py < core.bottom) {
      return 255;
    }
  }

  const int n = options_.antialias ? options_.samplesPerAxis : 1;
  const float step = 1.0f / static_cast<float>(n);
  int covered = 0;
  for (int sy = 0; sy < n; ++sy) {
    const float fy = static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) * step;
    for (int sx = 0; sx < n; ++sx) {
      const float fx = static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) * step;
      if (insideRoundRect(fx, fy, rc, radius)) ++covered;
    }
  }
  return toByte(covered * 255 / (n * n));
}

std::uint8_t SoftwareBackend::clipCoverage(int x, int y) const {
  if (!state_.clipValid) return 255;
  if (state_.clipRadius <= 0) return 255;  // 已在 forEachSpan 里夹过
  return roundRectCoverage(x, y, state_.clip, static_cast<float>(state_.clipRadius));
}

// ── 基础绘制 ──

void SoftwareBackend::blendPixel(int x, int y, Color color) {
  blendCovered(target_, x, y, color, 255);
}

void SoftwareBackend::blendRow(int x0, int x1, int y, Color color, std::uint8_t coverage) {
  for (int x = x0; x < x1; ++x) blendCovered(target_, x, y, color, coverage);
}

void SoftwareBackend::fillRect(const Rect& rc, Color color) {
  if (rc.isEmpty() || colorAlpha(color) == 0) return;
  forEachSpan(rc, [&](int x0, int x1, int y) {
    if (state_.clipRadius <= 0) {
      blendRow(x0, x1, y, color, 255);
      return;
    }
    for (int x = x0; x < x1; ++x) {
      const std::uint8_t cov = clipCoverage(x, y);
      if (cov > 0) blendCovered(target_, x, y, color, cov);
    }
  });
}

void SoftwareBackend::fillRoundRectInternal(const Rect& rc, Color color, float radius) {
  if (rc.isEmpty() || colorAlpha(color) == 0) return;
  const Rect surfaceRect = toSurface(rc);
  forEachSpan(rc, [&](int x0, int x1, int y) {
    for (int x = x0; x < x1; ++x) {
      std::uint8_t cov = roundRectCoverage(x, y, surfaceRect, radius);
      if (cov == 0) continue;
      if (state_.clipRadius > 0) {
        cov = static_cast<std::uint8_t>(static_cast<int>(cov) * clipCoverage(x, y) / 255);
        if (cov == 0) continue;
      }
      blendCovered(target_, x, y, color, cov);
    }
  });
}

void SoftwareBackend::fillRoundRect(const Rect& rc, Color color, int radius) {
  if (rc.isEmpty()) return;
  // 铁律 #4：半径夹到 min(w,h)/2，否则两个圆角会互相吃掉
  const int maxRadius = std::min(rc.width(), rc.height()) / 2;
  const float r = static_cast<float>(radius < 0 ? 0 : std::min(radius, maxRadius));
  fillRoundRectInternal(rc, color, r);
}

void SoftwareBackend::strokeRoundRectInternal(const Rect& rc, Color color, float radius,
                                              int lineWidth) {
  // 铁律 #3：描边向内。做法是「外圈减去内圈」，而不是沿边界线采样，
  // 这样 lineWidth=1 时恰好占满最外 1 像素，不跨到外面。
  if (rc.isEmpty() || colorAlpha(color) == 0) return;
  const int w = std::min(lineWidth, std::min(rc.width(), rc.height()) / 2 + 1);
  if (w <= 0) return;

  const Rect outer = rc;
  const Rect inner = rc.inset(w, w, w, w);
  const float innerRadius = std::max(0.0f, radius - static_cast<float>(w));
  const Rect outerSurface = toSurface(outer);
  const Rect innerSurface = toSurface(inner);

  forEachSpan(rc, [&](int x0, int x1, int y) {
    for (int x = x0; x < x1; ++x) {
      const std::uint8_t outerCov = roundRectCoverage(x, y, outerSurface, radius);
      if (outerCov == 0) continue;

      std::uint8_t cov = outerCov;
      if (!inner.isEmpty()) {
        const std::uint8_t innerCov = roundRectCoverage(x, y, innerSurface, innerRadius);
        if (innerCov >= outerCov) continue;
        cov = static_cast<std::uint8_t>(outerCov - innerCov);
      }
      if (cov == 0) continue;
      if (state_.clipRadius > 0) {
        cov = static_cast<std::uint8_t>(static_cast<int>(cov) * clipCoverage(x, y) / 255);
        if (cov == 0) continue;
      }
      blendCovered(target_, x, y, color, cov);
    }
  });
}

void SoftwareBackend::strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth) {
  if (rc.isEmpty() || lineWidth <= 0) return;
  const int maxRadius = std::min(rc.width(), rc.height()) / 2;
  const float r = static_cast<float>(radius < 0 ? 0 : std::min(radius, maxRadius));
  strokeRoundRectInternal(rc, color, r, lineWidth);
}

void SoftwareBackend::fillGradientSpan(const Rect& rc, Color top, Color bottom, float radius) {
  if (rc.isEmpty()) return;
  const Rect surfaceRect = toSurface(rc);
  const int height = rc.height();

  forEachSpan(rc, [&](int x0, int x1, int y) {
    // 铁律 #5：top 对应 rc.top、bottom 对应 rc.bottom，含首末行。
    // 用「距顶的行数」而不是表面坐标，平移过的渐变才不会跟着窗口走。
    const int localY = y - surfaceRect.top;
    const int t = height <= 1 ? 0 : clampInt(localY * 255 / (height - 1), 0, 255);
    const Color row = lerpColor(top, bottom, t);

    for (int x = x0; x < x1; ++x) {
      std::uint8_t cov = roundRectCoverage(x, y, surfaceRect, radius);
      if (cov == 0) continue;
      if (state_.clipRadius > 0) {
        cov = static_cast<std::uint8_t>(static_cast<int>(cov) * clipCoverage(x, y) / 255);
        if (cov == 0) continue;
      }
      blendCovered(target_, x, y, row, cov);
    }
  });
}

void SoftwareBackend::drawGradientRect(const Rect& rc, Color top, Color bottom) {
  fillGradientSpan(rc, top, bottom, 0.0f);
}

void SoftwareBackend::drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) {
  if (rc.isEmpty()) return;
  const int maxRadius = std::min(rc.width(), rc.height()) / 2;
  const float r = static_cast<float>(radius < 0 ? 0 : std::min(radius, maxRadius));
  fillGradientSpan(rc, top, bottom, r);
}

void SoftwareBackend::fillCheckerboard(const Rect& rc, Color colorA, Color colorB, int cell) {
  if (rc.isEmpty()) return;
  if (cell <= 0) {
    fillRect(rc, colorA);
    return;
  }

  // 铁律 #6：相位锚在坐标系原点，不锚在 rc。换算到世界坐标再取格号，
  // 这样内容平移时底纹保持不动（拖动图片时才有「格子固定」的感觉）。
  forEachSpan(rc, [&](int x0, int x1, int y) {
    const int wy = y - state_.ty;
    const bool oddRow = ((wy / cell) & 1) != 0;
    for (int x = x0; x < x1; ++x) {
      const int wx = x - state_.tx;
      const bool oddCol = ((wx / cell) & 1) != 0;
      const Color color = (oddRow != oddCol) ? colorB : colorA;
      std::uint8_t cov = 255;
      if (state_.clipRadius > 0) cov = clipCoverage(x, y);
      if (cov > 0) blendCovered(target_, x, y, color, cov);
    }
  });
}

// ── 文本 ──

void SoftwareBackend::blendCoverageMask(const Rect& rc, const std::uint8_t* coverage, int stride,
                                        Color color) {
  if (rc.isEmpty() || coverage == nullptr || stride <= 0) return;
  if (colorAlpha(color) == 0) return;

  // 蒙版坐标是「rc 的行列」，与 forEachSpan 给出的**表面**行号不同，所以这里
  // 自己做裁剪而不走 forEachSpan：后者只吐行区间，丢掉了「源行从哪里开始」，
  // 而蒙版必须按源行取样，否则裁剪掉顶部之后整块字形会向上错位。
  const Rect surface = toSurface(rc).intersect(Rect::fromSize(target_.width(), target_.height()));
  const Rect clipped = surface.intersect(state_.clipValid ? state_.clip : surface);
  if (clipped.isEmpty()) {
    ++culled_;
    return;
  }

  const int sourceLeft = clipped.left - surface.left;
  const int sourceTop = clipped.top - surface.top;

  for (int y = clipped.top; y < clipped.bottom; ++y) {
    const int row = sourceTop + (y - clipped.top);
    const std::uint8_t* src = coverage + static_cast<size_t>(row) * static_cast<size_t>(stride) +
                              static_cast<size_t>(sourceLeft);

    if (state_.clipRadius <= 0) {
      for (int x = clipped.left, i = 0; x < clipped.right; ++x, ++i) {
        const std::uint8_t cov = src[i];
        if (cov == 0) continue;
        blendCovered(target_, x, y, color, cov);
      }
      continue;
    }
    for (int x = clipped.left, i = 0; x < clipped.right; ++x, ++i) {
      std::uint8_t cov = src[i];
      if (cov == 0) continue;
      cov = static_cast<std::uint8_t>(static_cast<int>(cov) * clipCoverage(x, y) / 255);
      if (cov == 0) continue;
      blendCovered(target_, x, y, color, cov);
    }
  }
}

void SoftwareBackend::drawText(const Rect& rc, std::string_view text, Color color, const Font& font,
                               TextAlign align) {
  if (text.empty() || rc.isEmpty()) return;
  if (textRenderer_ != nullptr) {
    // 委托给渲染器，它回头调本后端的 fillRect —— 变换与裁剪只应用一次
    textRenderer_->draw(*this, rc, text, color, font, align);
    return;
  }
  if (!options_.placeholderText) return;

  // 占位字形：与 MockTextRenderer 同形（每字符一个块、留 1 像素缝、
  // 顶部空 20%），这样「没有字体时不显示」与「有 Mock 字体时」的像素断言
  // 可以共用同一组期望值。
  const int perChar = font.isValid() ? std::max(1, font.pixelHeight() / 2) : 6;
  const int width = static_cast<int>(utf8Length(text)) * perChar;
  int left = rc.left;
  if (align == TextAlign::kCenter) {
    left = rc.left + (rc.width() - width) / 2;
  } else if (align == TextAlign::kRight) {
    left = rc.right - width;
  }
  drawTextRun(Rect{left, rc.top, left + width, rc.bottom}, text, color, font);
}

void SoftwareBackend::drawTextRun(const Rect& rc, std::string_view text, Color color,
                                  const Font& font) {
  if (text.empty() || rc.isEmpty()) return;
  if (textRenderer_ != nullptr) {
    textRenderer_->drawRun(*this, rc, text, color, font);
    return;
  }
  if (!options_.placeholderText) return;

  const int perChar = font.isValid() ? std::max(1, font.pixelHeight() / 2) : 6;
  const int top = rc.top + rc.height() / 5;
  const size_t count = utf8Length(text);
  for (size_t i = 0; i < count; ++i) {
    const int x0 = rc.left + static_cast<int>(i) * perChar;
    const int x1 = x0 + perChar - 1;
    if (x1 <= x0) continue;
    fillRect(Rect{x0, top, x1, rc.bottom}, color);
  }
}

int SoftwareBackend::measureText(std::string_view text, const Font& font) {
  if (textRenderer_ != nullptr) return textRenderer_->measure(text, font).width;
  const int perChar = font.isValid() ? std::max(1, font.pixelHeight() / 2) : 6;
  return static_cast<int>(utf8Length(text)) * perChar;
}

int SoftwareBackend::advanceText(std::string_view text, const Font& font) {
  if (textRenderer_ != nullptr) return textRenderer_->advance(text, font);
  return measureText(text, font);
}

std::vector<int> SoftwareBackend::glyphBounds(std::string_view text, const Font& font) {
  if (textRenderer_ != nullptr) return textRenderer_->glyphBounds(text, font);
  return {};
}

int SoftwareBackend::textHeight(const Font& font) {
  if (textRenderer_ != nullptr) return textRenderer_->measure(std::string_view{}, font).height;
  return font.isValid() ? font.pixelHeight() : 12;
}

int SoftwareBackend::lineHeight(const Font& font) {
  if (textRenderer_ != nullptr) return textRenderer_->measure(std::string_view{}, font).lineHeight;
  return font.isValid() ? (font.pixelHeight() * 4 / 3) : 16;
}

// ── 图标 ──

void SoftwareBackend::fillThickLine(int x0, int y0, int x1, int y1, int thickness, Color color) {
  const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
  const int half = thickness / 2;
  if (steps == 0) {
    fillRect(Rect{x0 - half, y0 - half, x0 - half + thickness, y0 - half + thickness}, color);
    return;
  }
  // 沿主轴步进、每步盖一个方块：不做抗锯齿，图标在三个后端之间
  // 也不会因为曲线拟合不同而出现形状差异（形状一致性优先于平滑）。
  for (int i = 0; i <= steps; ++i) {
    const int x = x0 + (x1 - x0) * i / steps;
    const int y = y0 + (y1 - y0) * i / steps;
    fillRect(Rect{x - half, y - half, x - half + thickness, y - half + thickness}, color);
  }
}

void SoftwareBackend::drawIcon(IconId id, const Rect& rc, Color color) {
  if (rc.isEmpty() || id == IconId::kNone) return;

  const int size = std::min(rc.width(), rc.height());
  if (size <= 0) return;
  const int thickness = std::max(1, size / 10);

  // 图标在 rc 内按墨迹居中：留 1/8 边距，与 AiUI 的观感一致
  const int inset = size / 8;
  const Rect box = Rect{rc.left + (rc.width() - size) / 2, rc.top + (rc.height() - size) / 2,
                        rc.left + (rc.width() - size) / 2 + size,
                        rc.top + (rc.height() - size) / 2 + size}
                       .inset(inset, inset, inset, inset);
  if (box.isEmpty()) return;

  const int l = box.left;
  const int t = box.top;
  const int r = box.right;
  const int b = box.bottom;
  const int cx = (l + r) / 2;
  const int cy = (t + b) / 2;

  switch (id) {
    case IconId::kCheck:
      fillThickLine(l + (r - l) / 6, cy, cx - (r - l) / 10, b - (b - t) / 4, thickness, color);
      fillThickLine(cx - (r - l) / 10, b - (b - t) / 4, r - (r - l) / 8, t + (b - t) / 4, thickness,
                    color);
      break;
    case IconId::kClose:
      fillThickLine(l, t, r, b, thickness, color);
      fillThickLine(r, t, l, b, thickness, color);
      break;
    case IconId::kMinimize:
      fillRect(Rect{l, cy - thickness / 2, r, cy - thickness / 2 + thickness}, color);
      break;
    case IconId::kMaximize:
      fillRect(Rect{l, t, r, t + thickness}, color);
      fillRect(Rect{l, b - thickness, r, b}, color);
      fillRect(Rect{l, t, l + thickness, b}, color);
      fillRect(Rect{r - thickness, t, r, b}, color);
      break;
    case IconId::kRestore: {
      // 标准「还原」字形：前面一个完整方框，后面那个只露出顶边与右边。
      //
      // 早先画的是两个都完整的方框，四条边互相压在一起，11px 下糊成一团黑，
      // 看起来不像系统图标。系统的 ChromeRestore 只画后框露出来的那两条边。
      const int offset = (r - l) / 4;
      // 后框：顶边与右边（左下被前框盖住，不画）
      fillRect(Rect{l + offset, t, r, t + thickness}, color);
      fillRect(Rect{r - thickness, t, r, b - offset}, color);
      // 前框：完整方框
      fillRect(Rect{l, t + offset, r - offset, t + offset + thickness}, color);
      fillRect(Rect{l, b - thickness, r - offset, b}, color);
      fillRect(Rect{l, t + offset, l + thickness, b}, color);
      fillRect(Rect{r - offset - thickness, t + offset, r - offset, b}, color);
      break;
    }
    case IconId::kChevronDown:
      fillThickLine(l, t + (b - t) / 3, cx, b - (b - t) / 6, thickness, color);
      fillThickLine(cx, b - (b - t) / 6, r, t + (b - t) / 3, thickness, color);
      break;
    case IconId::kChevronUp:
      fillThickLine(l, b - (b - t) / 3, cx, t + (b - t) / 6, thickness, color);
      fillThickLine(cx, t + (b - t) / 6, r, b - (b - t) / 3, thickness, color);
      break;
    case IconId::kChevronRight:
      fillThickLine(l + (r - l) / 3, t, r - (r - l) / 6, cy, thickness, color);
      fillThickLine(r - (r - l) / 6, cy, l + (r - l) / 3, b, thickness, color);
      break;
    case IconId::kSearch: {
      // 圆环 + 斜柄：环用固定 24 段，形状在三个后端里一致
      const int radius = std::min(r - l, b - t) * 3 / 8;
      const int ringThickness = std::max(1, thickness);
      for (int i = 0; i < 24; ++i) {
        const int x = cx - radius + radius * 2 * i / 23;
        // 用圆的方程求 y，避免浮点三角函数
        const int dx = x - cx;
        const int dy = isqrt(radius * radius - dx * dx);
        fillRect(Rect{x, cy - dy, x + 1, cy - dy + ringThickness}, color);
        fillRect(Rect{x, cy + dy - ringThickness, x + 1, cy + dy}, color);
      }
      fillThickLine(cx + radius * 7 / 10, cy + radius * 7 / 10, r, b, thickness + 1, color);
      break;
    }
    case IconId::kFolder: {
      // 上沿一个凸起的标签，再铺主体
      fillRect(Rect{l, t, l + (r - l) / 2, t + thickness * 2}, color);
      fillRect(Rect{l, t + thickness, r, b}, color);
      break;
    }
    case IconId::kFile:
      fillRect(Rect{l, t, r, t + thickness}, color);
      fillRect(Rect{l, b - thickness, r, b}, color);
      fillRect(Rect{l, t, l + thickness, b}, color);
      fillRect(Rect{r - thickness, t, r, b}, color);
      fillRect(Rect{l, t + (b - t) / 2 - thickness / 2, r - (r - l) / 3, t + (b - t) / 2 - thickness / 2 + thickness}, color);
      break;
    case IconId::kNone:
      break;
  }
}

// ── 位图 ──
//
// 这个后端不认识任何图像格式，也不需要认识：平台层负责把 HICON / 文件 /
// 网络字节解码成一支 0xAARRGGBB 的 Surface，再用 Image 交过来。契约写在
// painter.h 的 Image 注释里——handle 就是 Surface*。
//
// 采样用最近邻而不是双线性：走这条路径的基本都是图标与界面位图，模糊比
// 锯齿更难看；而且整数映射让三个后端的像素结果彼此可比。

namespace {

const Surface* imageSurface(const Image& image) {
  return static_cast<const Surface*>(image.handle());
}

}  // namespace

void SoftwareBackend::drawImage(const Image& image, const Rect& rc) {
  drawImageScaled(image, rc, Rect{}, ImageSmoothing::kNearest, 1.0f);
}

void SoftwareBackend::drawImage(const Image& image, const Rect& rc, const Rect& source) {
  drawImageScaled(image, rc, source, ImageSmoothing::kNearest, 1.0f);
}

void SoftwareBackend::drawImageScaled(const Image& image, const Rect& rc, const Rect& source,
                                      ImageSmoothing smoothing, float alpha) {
  (void)smoothing;

  const Surface* from = imageSurface(image);
  if (from == nullptr || !from->valid()) {
    ++unsupported_;
    return;
  }
  if (rc.isEmpty() || alpha <= 0.0f) return;

  const Rect whole = from->bounds();
  const Rect srcRect = source.isEmpty() ? whole : source.intersect(whole);
  if (srcRect.isEmpty()) return;

  const float clamped = std::min(1.0f, alpha);
  const std::uint8_t alphaByte = toByte(static_cast<int>(clamped * 255.0f + 0.5f));
  if (alphaByte == 0) return;

  // dst 是本图元在表面上的落点；rc 可能已被 translate 过，所以映射要
  // 只用「相对 dst 的偏移」，不能拿 rc.left 直接当基准。
  const Rect dst = toSurface(rc);
  if (dst.width() <= 0 || dst.height() <= 0) return;

  forEachSpan(rc, [&](int x0, int x1, int y) {
    const int sy = srcRect.top + (y - dst.top) * srcRect.height() / dst.height();
    const std::uint32_t* row = from->row(sy);
    if (row == nullptr) return;
    for (int x = x0; x < x1; ++x) {
      const int sx = srcRect.left + (x - dst.left) * srcRect.width() / dst.width();
      const std::uint32_t pixel = row[sx];
      // 位图自带的 alpha 与整体淡入淡出相乘，得到最终覆盖率。
      // blendCovered 还会再乘一次 colorAlpha，所以颜色本身强制不透明，
      // 否则 alpha 会被平方（半透明图会明显偏淡）。
      const int coverage = static_cast<int>(pixel >> 24) * alphaByte / 255;
      if (coverage <= 0) continue;
      blendCovered(target_, x, y, pixel | 0xFF000000u, static_cast<std::uint8_t>(coverage));
    }
  });
}

// ── 裁剪与变换 ──

void SoftwareBackend::setClip(const Rect& rc) {
  const Rect surface = toSurface(rc);
  state_.clip = surface;
  state_.clipValid = !surface.isEmpty();
  state_.clipRadius = -1;
}

void SoftwareBackend::resetClip() {
  state_.clip = Rect::fromSize(target_.width(), target_.height());
  state_.clipValid = true;
  state_.clipRadius = -1;
}

void SoftwareBackend::clipRect(const Rect& rc) {
  const Rect surface = toSurface(rc);
  // 与当前裁剪区求交（铁律 #7）：clipRect 是「再收紧一层」，不是「换成这个」
  if (state_.clipValid) {
    state_.clip = state_.clip.intersect(surface);
  } else {
    state_.clip = surface;
    state_.clipValid = true;
  }
  // 矩形裁剪会把外层的圆角变成硬边——这是正确行为：
  // 求交后的形状本来就是「圆角 ∩ 矩形」，硬边是它的准确描述。
  state_.clipRadius = -1;
}

void SoftwareBackend::clipRoundRect(const Rect& rc, int radius) {
  const Rect surface = toSurface(rc);
  if (state_.clipValid) {
    state_.clip = state_.clip.intersect(surface);
  } else {
    state_.clip = surface;
    state_.clipValid = true;
  }
  // 只保留一层圆角：真正的「圆角 ∩ 圆角」没有闭式表示，
  // 假造一个会把边界画错。嵌套圆角裁剪在控件里不该出现。
  const int maxRadius = std::min(surface.width(), surface.height()) / 2;
  state_.clipRadius = radius < 0 ? 0 : std::min(radius, maxRadius);
}

void SoftwareBackend::translate(int dx, int dy) {
  state_.tx += dx;
  state_.ty += dy;
}

void SoftwareBackend::rotate(float degrees) {
  // 离线后端不支持任意旋转：图片按 90° 倍数旋转由调用方先重排像素，
  // 走到这里说明有控件直接调了 rotate。计数而不静默画错。
  (void)degrees;
  ++unsupported_;
}

void SoftwareBackend::save() { stack_.push_back(state_); }

void SoftwareBackend::restore() {
  // 栈空时什么都不做而不是崩：控件里 save/restore 没配对是 bug，
  // 但测试进程不该因此直接挂掉，留给 unsupported/culled 统计去暴露。
  if (stack_.empty()) return;
  state_ = stack_.back();
  stack_.pop_back();
}

Rect SoftwareBackend::currentClip() const {
  if (!state_.clipValid) return Rect{};
  return state_.clip.moved(-state_.tx, -state_.ty);
}

// ── 测试辅助 ──

std::vector<std::uint8_t> encodeSurfaceRaw(const Surface& surface) {
  if (!surface.valid()) return {};
  const size_t bytes = static_cast<size_t>(surface.width()) * surface.height() * 4;
  std::vector<std::uint8_t> out(bytes);
  std::memcpy(out.data(), surface.pixels(), bytes);
  return out;
}

}  // namespace pyui
