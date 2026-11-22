#include "pyui/text_renderer.h"

#include <algorithm>

namespace pyui {
namespace {

// 没有绑定 TextRenderer 时的回退度量。
//
// 为什么要有回退而不是返回 0：布局测试、离线预览、以及「界面还没绑定字体」
// 的早期启动阶段都会走到这里。返回 0 宽会让所有 wrap 控件塌成一条线，症状
// 极难定位；给一组与 MockTextRenderer 一致的确定性数值，至少布局是稳的。
constexpr int kFallbackPixelHeight = 12;
constexpr int kFallbackLineHeight = 16;
constexpr float kFallbackAdvanceRatio = 0.5f;

int fallbackPixelSize(float pointSize, int dpi) {
  // 1pt = 1/72 英寸；像素 = 点 * dpi / 72，四舍五入
  const float pixels = pointSize * static_cast<float>(dpi > 0 ? dpi : 96) / 72.0f;
  return static_cast<int>(pixels + 0.5f);
}

}  // namespace

MockTextRenderer::MockTextRenderer(float advanceRatio) : advanceRatio_(advanceRatio) {}

Font MockTextRenderer::createFont(const FontSpec& spec) {
  MockFont entry;
  entry.pixelHeight = fallbackPixelSize(spec.pointSize, spec.dpi);
  // 行高固定为字号的 4/3：真字体的行距大多在这个比例附近，
  // 用一个整数比例能让测试期望值好写，也不至于紧贴。
  entry.lineHeight = entry.pixelHeight * 4 / 3;
  if (entry.lineHeight <= entry.pixelHeight) entry.lineHeight = entry.pixelHeight + 4;

  fonts_.push_back(entry);
  // 句柄用下标 +1 编码，避免 nullptr 与「0 号字体」混淆
  const int index = static_cast<int>(fonts_.size());
  return Font(reinterpret_cast<void*>(static_cast<intptr_t>(index)), entry.pixelHeight);
}

void MockTextRenderer::destroyFont(const Font& font) {
  (void)font;
}

int MockTextRenderer::advanceOf(std::string_view text, const Font& font) const {
  const int pixelHeight = font.isValid() ? font.pixelHeight() : kFallbackPixelHeight;
  const int perChar = static_cast<int>(static_cast<float>(pixelHeight) * advanceRatio_ + 0.5f);
  return static_cast<int>(utf8Length(text)) * (perChar > 0 ? perChar : 1);
}

TextMetrics MockTextRenderer::measure(std::string_view text, const Font& font) {
  TextMetrics metrics;
  metrics.width = advanceOf(text, font);
  metrics.height = font.isValid() ? font.pixelHeight() : kFallbackPixelHeight;
  metrics.lineHeight = font.isValid() ? (font.pixelHeight() * 4 / 3) : kFallbackLineHeight;
  if (metrics.lineHeight <= 0) metrics.lineHeight = metrics.height;
  metrics.ascent = metrics.height;
  metrics.descent = 0;
  return metrics;
}

int MockTextRenderer::advance(std::string_view text, const Font& font) {
  return advanceOf(text, font);
}

std::vector<int> MockTextRenderer::glyphBounds(std::string_view text, const Font& font) {
  const int perChar = [&] {
    const int pixelHeight = font.isValid() ? font.pixelHeight() : kFallbackPixelHeight;
    const int value = static_cast<int>(static_cast<float>(pixelHeight) * advanceRatio_ + 0.5f);
    return value > 0 ? value : 1;
  }();
  const size_t count = utf8Length(text);
  std::vector<int> out(count * 2);
  for (size_t i = 0; i < count; ++i) {
    out[i * 2] = static_cast<int>(i) * perChar;
    out[i * 2 + 1] = static_cast<int>(i + 1) * perChar;
  }
  return out;
}

void MockTextRenderer::draw(PaintBackend& backend, const Rect& rc, std::string_view text,
                            Color color, const Font& font, TextAlign align) {
  if (text.empty() || rc.isEmpty()) return;
  const int width = advanceOf(text, font);
  int left = rc.left;
  if (align == TextAlign::kCenter) {
    left = rc.left + (rc.width() - width) / 2;
  } else if (align == TextAlign::kRight) {
    left = rc.right - width;
  }
  drawRun(backend, Rect{left, rc.top, left + width, rc.bottom}, text, color, font);
}

void MockTextRenderer::drawRun(PaintBackend& backend, const Rect& rc, std::string_view text,
                               Color color, const Font& font) {
  if (text.empty() || rc.isEmpty()) return;
  // 方块字形：每字符一个实心块，块间留 1 像素缝，便于在像素测试里数清字符。
  // 顶部留 20% 空、底部不留——与真字体的「上沿有行距、基线贴底」一致，
  // 这样垂直居中的断言对真字体也成立。
  const int perChar = [&] {
    const int pixelHeight = font.isValid() ? font.pixelHeight() : kFallbackPixelHeight;
    const int value = static_cast<int>(static_cast<float>(pixelHeight) * advanceRatio_ + 0.5f);
    return value > 0 ? value : 1;
  }();
  const int top = rc.top + rc.height() / 5;
  const size_t count = utf8Length(text);
  for (size_t i = 0; i < count; ++i) {
    const int x0 = rc.left + static_cast<int>(i) * perChar;
    const int x1 = x0 + perChar - 1;
    if (x1 <= x0) continue;
    backend.fillRect(Rect{x0, top, x1, rc.bottom}, color);
  }
}

// ── FontCache ──

FontCache& FontCache::instance() {
  static FontCache cache;
  return cache;
}

FontCache::~FontCache() = default;

void FontCache::setTextRenderer(TextRenderer* renderer) {
  if (renderer_ == renderer) return;
  clear();
  renderer_ = renderer;
}

void FontCache::clear() {
  if (renderer_ != nullptr) {
    for (const auto& entry : entries_) {
      if (entry.font.isValid()) renderer_->destroyFont(entry.font);
    }
  }
  entries_.clear();
}

Font FontCache::get(const FontSpec& spec) {
  if (renderer_ == nullptr) return Font{};
  for (auto& entry : entries_) {
    if (entry.spec.family == spec.family && entry.spec.pointSize == spec.pointSize &&
        entry.spec.bold == spec.bold && entry.spec.italic == spec.italic &&
        entry.spec.dpi == spec.dpi) {
      return entry.font;
    }
  }
  Font font = renderer_->createFont(spec);
  entries_.push_back(Entry{spec, font});
  return font;
}

// ── 测量入口 ──

int measureTextWidth(std::string_view text, const Font& font) {
  TextRenderer* renderer = FontCache::instance().textRenderer();
  if (renderer == nullptr) {
    const int pixels = font.isValid() ? font.pixelHeight() : kFallbackPixelHeight;
    const int perChar = static_cast<int>(static_cast<float>(pixels) * kFallbackAdvanceRatio + 0.5f);
    return static_cast<int>(utf8Length(text)) * (perChar > 0 ? perChar : 1);
  }
  return renderer->measure(text, font).width;
}

int textAdvance(std::string_view text, const Font& font) {
  TextRenderer* renderer = FontCache::instance().textRenderer();
  if (renderer == nullptr) return measureTextWidth(text, font);
  return renderer->advance(text, font);
}

int textInkHeight(const Font& font) {
  TextRenderer* renderer = FontCache::instance().textRenderer();
  if (renderer == nullptr) return font.isValid() ? font.pixelHeight() : kFallbackPixelHeight;
  return renderer->measure(std::string_view{}, font).height;
}

int textLineHeight(const Font& font) {
  TextRenderer* renderer = FontCache::instance().textRenderer();
  if (renderer == nullptr) return font.isValid() ? font.pixelHeight() : kFallbackLineHeight;
  return renderer->measure(std::string_view{}, font).lineHeight;
}

}  // namespace pyui
