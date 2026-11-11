// GDI+ 绘制后端：PaintBackend / Canvas / 字体
#include "backend.h"

#include <algorithm>
#include <vector>

namespace AiUI {
namespace os {
namespace windows {

using namespace Gdiplus;

namespace {

Gdiplus::Color toGdiplus(Color c) {
  return Gdiplus::Color(colorAlpha(c), colorRed(c), colorGreen(c), colorBlue(c));
}

Gdiplus::Rect toGdiplus(const Rect& rc) {
  return Gdiplus::Rect(rc.left, rc.top, rc.width(), rc.height());
}

// 圆角路径；半径过大时退化成直角
void buildRoundPath(GraphicsPath& path, const Rect& rc, int radius) {
  const int w = rc.width();
  const int h = rc.height();
  if (radius <= 0 || radius * 2 >= w || radius * 2 >= h) {
    path.AddRectangle(toGdiplus(rc));
    return;
  }
  const int d = radius * 2;
  path.AddArc(rc.left, rc.top, d, d, 180.0f, 90.0f);
  path.AddArc(rc.right - d, rc.top, d, d, 270.0f, 90.0f);
  path.AddArc(rc.right - d, rc.bottom - d, d, d, 0.0f, 90.0f);
  path.AddArc(rc.left, rc.bottom - d, d, d, 90.0f, 90.0f);
  path.CloseFigure();
}

Gdiplus::Font* asFont(const Font& font) {
  return static_cast<Gdiplus::Font*>(font.handle());
}

class GdiplusBackend : public PaintBackend {
 public:
  explicit GdiplusBackend(Graphics* g) : g_(g) {}

  void fillRect(const Rect& rc, Color color) override {
    if (!g_ || rc.isEmpty()) return;
    SolidBrush brush(toGdiplus(color));
    g_->FillRectangle(&brush, toGdiplus(rc));
  }

  void fillRoundRect(const Rect& rc, Color color, int radius) override {
    if (!g_ || rc.isEmpty()) return;
    GraphicsPath path;
    buildRoundPath(path, rc, radius);
    SolidBrush brush(toGdiplus(color));
    g_->FillPath(&brush, &path);
  }

  void strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth) override {
    if (!g_ || rc.isEmpty()) return;
    const int inset = (std::max)(0, lineWidth / 2);
    const Rect inner = rc.deflated(inset, inset);
    if (inner.isEmpty()) return;
    GraphicsPath path;
    buildRoundPath(path, inner, (std::max)(0, radius - inset));
    Pen pen(toGdiplus(color), static_cast<REAL>(lineWidth));
    g_->DrawPath(&pen, &path);
  }

  void drawGradientRect(const Rect& rc, Color top, Color bottom) override {
    if (!g_ || rc.isEmpty()) return;
    // 渐变画到 bottom+1，避免最后一行取不到末端颜色
    LinearGradientBrush brush(toGdiplus(Rect{rc.left, rc.top, rc.right, rc.bottom + 1}),
                              toGdiplus(top), toGdiplus(bottom), LinearGradientModeVertical);
    g_->FillRectangle(&brush, toGdiplus(rc));
  }

  void drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) override {
    if (!g_ || rc.isEmpty()) return;
    GraphicsPath path;
    buildRoundPath(path, rc, radius);
    LinearGradientBrush brush(toGdiplus(Rect{rc.left, rc.top, rc.right, rc.bottom + 1}),
                              toGdiplus(top), toGdiplus(bottom), LinearGradientModeVertical);
    g_->FillPath(&brush, &path);
  }

  void drawText(const Rect& rc, const std::wstring& text, Color color, const Font& font,
                TextAlign align) override {
    Gdiplus::Font* f = asFont(font);
    if (!g_ || !f || text.empty() || rc.isEmpty()) return;

    StringFormat fmt;
    fmt.SetAlignment(align == TextAlign::kCenter   ? StringAlignmentCenter
                     : align == TextAlign::kRight ? StringAlignmentFar
                                                  : StringAlignmentNear);
    fmt.SetLineAlignment(StringAlignmentCenter);
    // NoWrap + NoClip：单行绘制，超宽由调用方自行省略
    fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip |
                       StringFormatFlagsMeasureTrailingSpaces);

    SolidBrush brush(toGdiplus(color));
    g_->SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g_->DrawString(text.c_str(), static_cast<INT>(text.size()), f,
                   RectF(static_cast<REAL>(rc.left), static_cast<REAL>(rc.top),
                         static_cast<REAL>(rc.width()), static_cast<REAL>(rc.height())),
                   &fmt, &brush);
  }

  int measureText(const std::wstring& text, int from, int length, const Font& font) override {
    Gdiplus::Font* f = asFont(font);
    if (!g_ || !f || length <= 0) return 0;
    const int n = static_cast<int>(text.size());
    if (from < 0) from = 0;
    if (from >= n) return 0;
    if (from + length > n) length = n - from;
    if (length <= 0) return 0;

    RectF bounds;
    g_->MeasureString(text.c_str() + from, length, f, PointF(0, 0), &bounds);
    // MeasureString 偏小，整数化后尾部字符会被 DrawString 裁掉，留 4px 余量
    return static_cast<int>(bounds.Width + 0.5f) + 4;
  }

  int textHeight(const Font& font) override { return font.pixelHeight(); }

  void drawIcon(IconId id, const Rect& rc, Color color) override {
    if (!g_) return;
    drawIconGlyph(g_, id, rc, color);
  }

  void drawImage(const Image& image, const Rect& rc) override {
    Gdiplus::Bitmap* bmp = asBitmap(image);
    if (!g_ || !bmp || rc.isEmpty()) return;
    g_->DrawImage(bmp, toGdiplus(rc));
  }

  void setClip(const Rect& rc) override {
    if (g_) g_->SetClip(toGdiplus(rc));
  }
  void resetClip() override {
    if (g_) g_->ResetClip();
  }
  void clipRect(const Rect& rc) override {
    if (!g_) return;
    // Intersect 与当前裁剪区求交，配合 save/restore 逐层收缩
    g_->SetClip(toGdiplus(rc), CombineModeIntersect);
  }

  void translate(int dx, int dy) override {
    if (g_) g_->TranslateTransform(static_cast<REAL>(dx), static_cast<REAL>(dy));
  }
  void save() override {
    if (!g_) return;
    states_.push_back(g_->Save());
  }
  void restore() override {
    if (!g_ || states_.empty()) return;
    g_->Restore(states_.back());
    states_.pop_back();
  }

 private:
  Graphics* g_ = nullptr;
  std::vector<GraphicsState> states_;
};

// ── 离屏画布 ──
// 用 DIB section 而不是 Gdiplus::Bitmap：GDI+ 把位图贴回窗口 DC 时走软件合成，
// 满窗一次要 120ms 上下；换成 BitBlt 后是硬件 blit，动画期间才不会掉帧。
class DibCanvas : public Canvas {
 public:
  DibCanvas(HDC target, const Rect& rc, HDC memdc, HBITMAP dib, void* bits)
      : target_(target), rc_(rc), memdc_(memdc), dib_(dib), bits_(bits) {
    graphics_ = Graphics::FromHDC(memdc_);
    if (graphics_) {
      graphics_->SetSmoothingMode(SmoothingModeAntiAlias);
      graphics_->SetPixelOffsetMode(PixelOffsetModeHalf);
      graphics_->SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
      // 调用方用窗口坐标绘制，位图里只装 rc 这一块
      graphics_->TranslateTransform(static_cast<REAL>(-rc.left), static_cast<REAL>(-rc.top));
    }
    backend_ = std::make_unique<GdiplusBackend>(graphics_);
    painter_ = std::make_unique<Painter>(*backend_);
  }

  ~DibCanvas() override {
    painter_.reset();
    backend_.reset();
    delete graphics_;
  }

  Painter& painter() override { return *painter_; }

  void commit() override {
    if (!target_ || !memdc_) return;
    // DIB 内容已经是最终像素，直接盖过去，不需要 alpha 合成
    ::BitBlt(target_, rc_.left, rc_.top, rc_.width(), rc_.height(), memdc_, 0, 0, SRCCOPY);
  }

 private:
  HDC target_ = nullptr;
  Rect rc_{};
  HDC memdc_ = nullptr;
  HBITMAP dib_ = nullptr;
  void* bits_ = nullptr;
  Graphics* graphics_ = nullptr;
  std::unique_ptr<GdiplusBackend> backend_;
  std::unique_ptr<Painter> painter_;
};

}  // namespace

CanvasCache::~CanvasCache() {
  if (memdc_) {
    ::SelectObject(memdc_, oldBitmap_);
    ::DeleteDC(memdc_);
  }
  if (dib_) ::DeleteObject(dib_);
}

std::unique_ptr<Canvas> CanvasCache::create(HDC target, const Rect& rc) {
  if (!target || rc.isEmpty()) return nullptr;
  const int w = (std::max)(1, rc.width());
  const int h = (std::max)(1, rc.height());

  if (!dib_ || width_ < w || height_ < h) {
    // 一次性按较大尺寸分配，避免动画期间反复重建
    const int nw = (std::max)(w, width_);
    const int nh = (std::max)(h, height_);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = nw;
    bi.bmiHeader.biHeight = -nh;  // 负高度表示自上而下，避免 GDI 翻转
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(target, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) return nullptr;

    if (!memdc_) {
      memdc_ = ::CreateCompatibleDC(target);
      if (!memdc_) {
        ::DeleteObject(dib);
        return nullptr;
      }
    }
    HGDIOBJ old = ::SelectObject(memdc_, dib);
    if (oldBitmap_) ::DeleteObject(oldBitmap_);
    oldBitmap_ = static_cast<HBITMAP>(old);

    if (dib_) ::DeleteObject(dib_);
    dib_ = dib;
    bits_ = bits;
    width_ = nw;
    height_ = nh;
  }
  return std::make_unique<DibCanvas>(target, rc, memdc_, dib_, bits_);
}

ULONG_PTR g_token = 0;

bool ensureGdiplus() {
  if (g_token) return true;
  GdiplusStartupInput input;
  return GdiplusStartup(&g_token, &input, nullptr) == Ok;
}

void releaseGdiplus() {
  if (!g_token) return;
  GdiplusShutdown(g_token);
  g_token = 0;
}

std::unique_ptr<PaintBackend> makePaintBackend(Gdiplus::Graphics* g) {
  if (!g) return nullptr;
  g->SetSmoothingMode(SmoothingModeAntiAlias);
  g->SetPixelOffsetMode(PixelOffsetModeHalf);
  g->SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  return std::make_unique<GdiplusBackend>(g);
}

Font makeFont(const wchar_t* family, float pointSize, bool bold, int dpi) {
  if (!ensureGdiplus()) return Font();
  const REAL pixelSize = pointSize * static_cast<REAL>(dpi) / 72.0f;
  const int style = bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular;

  const Gdiplus::FontFamily preferred(family);
  const Gdiplus::FontFamily fallback(L"Microsoft YaHei UI");
  const Gdiplus::FontFamily* use = preferred.IsAvailable() ? &preferred : &fallback;

  auto* font = new Gdiplus::Font(use, pixelSize, style, Gdiplus::UnitPixel);
  if (!font->IsAvailable()) {
    delete font;
    return Font();
  }
  // 行高按 1.35 倍估算，与 GDI 的 tmHeight 观感接近
  const int lineHeight = static_cast<int>(pixelSize * 1.35f + 0.5f);
  return Font(font, lineHeight);
}

void releaseFont(void* handle) {
  delete static_cast<Gdiplus::Font*>(handle);
}

}  // namespace windows
}  // namespace os
}  // namespace AiUI
