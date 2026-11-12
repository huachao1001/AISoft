// 假平台后端实现
#include "mock_platform.h"

namespace aiui_test {

// ── RecordingBackend ──

void RecordingBackend::fillRect(const Rect& rc, Color color) {
  fills_.push_back(FillCall{map(rc), color});
}

void RecordingBackend::fillRoundRect(const Rect& rc, Color color, int radius) {
  fills_.push_back(FillCall{map(rc), color, radius});
}

void RecordingBackend::strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth) {
  (void)rc;
  (void)color;
  (void)radius;
  (void)lineWidth;
}

void RecordingBackend::drawGradientRect(const Rect& rc, Color top, Color bottom) {
  (void)bottom;
  fills_.push_back(FillCall{map(rc), top});
}

void RecordingBackend::drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) {
  (void)bottom;
  (void)radius;
  fills_.push_back(FillCall{map(rc), top});
}

void RecordingBackend::drawText(const Rect& rc, const std::wstring& text, Color color,
                                const AiUI::Font& font, AiUI::TextAlign align) {
  (void)rc;
  (void)text;
  (void)color;
  (void)font;
  (void)align;
}

int RecordingBackend::measureText(const std::wstring& text, int from, int length,
                                  const AiUI::Font& font) {
  (void)font;
  const int end = (std::min)(static_cast<int>(text.size()), from + length);
  return (std::max)(0, end - from) * 7;
}

int RecordingBackend::textHeight(const AiUI::Font& font) {
  return font.pixelHeight() > 0 ? font.pixelHeight() : 14;
}

void RecordingBackend::drawIcon(AiUI::IconId id, const Rect& rc, Color color) {
  (void)id;
  (void)rc;
  (void)color;
}

void RecordingBackend::drawImage(const AiUI::Image& image, const Rect& rc) {
  (void)image;
  (void)rc;
}

void RecordingBackend::setClip(const Rect& rc) { clip_ = rc; }

void RecordingBackend::resetClip() { clip_ = Rect{}; }

void RecordingBackend::clipRect(const Rect& rc) { clip_ = clip_.isEmpty() ? map(rc) : clip_.intersect(map(rc)); }

void RecordingBackend::clipRoundRect(const Rect& rc, int radius) {
  // 只记录成矩形裁剪：测试校验的是「有没有收缩区域」，圆角细节交给后端
  (void)radius;
  clipRect(rc);
}

void RecordingBackend::translate(int dx, int dy) {
  tx_ += dx;
  ty_ += dy;
}

void RecordingBackend::save() { stack_.push_back(State{tx_, ty_, clip_}); }

void RecordingBackend::restore() {
  if (stack_.empty()) return;
  const State state = stack_.back();
  stack_.pop_back();
  tx_ = state.tx;
  ty_ = state.ty;
  clip_ = state.clip;
}

void FakeCanvas::commit() {
  ++commits_;
  if (owner_) owner_->recordCommit(backend_, area_, commits_);
}

// ── FakeWindow ──

std::unique_ptr<AiUI::Canvas> FakeWindow::createCanvas(const Rect& area) {
  // 每次绘制从干净的后端状态开始，填充记录只包含本次内容
  backend_ = RecordingBackend{};
  auto canvas = std::make_unique<FakeCanvas>(backend_, area);
  canvas->setOwner(this);
  return canvas;
}

void FakeWindow::recordCommit(const RecordingBackend& backend, const Rect& area, int commits) {
  lastFills_ = backend.fills();
  lastArea_ = area;
  lastCommits_ = commits;
  lastTranslateX_ = backend.translateX();
  lastTranslateY_ = backend.translateY();
}

void FakeWindow::setBounds(const Rect& bounds) {
  const Size before{rect_.width(), rect_.height()};
  rect_ = Rect{0, 0, bounds.width(), bounds.height()};
  if (before.width != rect_.width() || before.height != rect_.height()) {
    host_.onResize(rect_.width(), rect_.height());
  }
}

// ── FakePlatform ──

AiUI::PlatformWindow* FakePlatform::createWindow(AiUI::NativeHandle parent,
                                                 const AiUI::WindowParams& params,
                                                 AiUI::WindowHost& host) {
  (void)parent;
  lastName_ = params.className;
  auto* window = new FakeWindow(host, dpi_);
  lastWindow_ = window;
  return window;
}

void FakePlatform::destroyWindow(AiUI::PlatformWindow* window) {
  delete window;
  ++destroyed_;
}

AiUI::Font FakePlatform::createFont(const wchar_t* family, float pointSize, bool bold, int dpi) {
  (void)family;
  (void)bold;
  // 字号换算成像素高度，测试只关心非零
  const int height = static_cast<int>(pointSize * dpi / 72.0f);
  return AiUI::Font(reinterpret_cast<void*>(1), height);
}

int FakePlatform::measureText(const std::wstring& text, const AiUI::Font& font) {
  (void)font;
  return static_cast<int>(text.size()) * charWidth_;
}

std::vector<int> FakePlatform::glyphBounds(const std::wstring& text, const AiUI::Font& font) {
  (void)font;
  std::vector<int> out(text.size() * 2, 0);
  if (brokenGlyphBounds_) {
    // 复刻 GDI+ 的退化行为：整串返回一个巨大的矩形，
    // 左缘为 0、右缘大到触发输入框的防御分支
    for (size_t i = 0; i < text.size(); ++i) {
      out[i * 2] = 0;
      out[i * 2 + 1] = 4000000;
    }
    return out;
  }
  for (size_t i = 0; i < text.size(); ++i) {
    out[i * 2] = static_cast<int>(i) * charWidth_;
    // 留出 1px 字形侧边距，光标落在侧边距里才不会压到墨迹
    out[i * 2 + 1] = static_cast<int>(i + 1) * charWidth() - 1;
  }
  return out;
}

int FakePlatform::advanceText(const std::wstring& text, const AiUI::Font& font) {
  (void)font;
  // 排版留白随文本变长非线性累积，模拟真实后端用 (loose-tight)/2 反推的偏差
  const int n = static_cast<int>(text.size());
  return n * charWidth_ + n * n / 4;
}

// ── ScopedFakePlatform ──

ScopedFakePlatform::ScopedFakePlatform() { AiUI::setPlatformForTesting(&fake_); }

ScopedFakePlatform::~ScopedFakePlatform() { AiUI::setPlatformForTesting(nullptr); }

}  // namespace aiui_test
