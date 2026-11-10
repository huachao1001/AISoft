#include "AUI/painter.h"

#include "AUI/font.h"
#include "AUI/theme.h"

namespace AUI {
namespace {

// 垂直渐变：GradientFill 需要 16 位色分量
void GradientFillV(HDC dc, const RECT& rc, COLORREF top, COLORREF bottom) {
  auto ch16 = [](BYTE v) { return static_cast<USHORT>(static_cast<UINT>(v) << 8); };
  auto put = [&](TRIVERTEX& tv, LONG x, LONG y, COLORREF c) {
    tv.x = x;
    tv.y = y;
    union {
      USHORT u;
      SHORT s;
    } r{}, g{}, b{}, a{};
    r.u = ch16(GetRValue(c));
    g.u = ch16(GetGValue(c));
    b.u = ch16(GetBValue(c));
    tv.Red = r.s;
    tv.Green = g.s;
    tv.Blue = b.s;
    a.u = 0xFF00;
    tv.Alpha = a.s;
  };
  TRIVERTEX tv[2]{};
  put(tv[0], rc.left, rc.top, top);
  put(tv[1], rc.right, rc.bottom, bottom);
  GRADIENT_RECT gr{0, 1};
  ::GradientFill(dc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

}  // namespace

void Painter::Fill(const RECT& rc, COLORREF color) const {
  HBRUSH b = ::CreateSolidBrush(color);
  ::FillRect(dc_, &rc, b);
  ::DeleteObject(b);
}

void Painter::FillRound(const RECT& rc, COLORREF color, int radius) const {
  HBRUSH b = ::CreateSolidBrush(color);
  HPEN p = ::CreatePen(PS_SOLID, 1, color);
  HBRUSH oldB = static_cast<HBRUSH>(::SelectObject(dc_, b));
  HPEN oldP = static_cast<HPEN>(::SelectObject(dc_, p));
  ::RoundRect(dc_, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
  ::SelectObject(dc_, oldB);
  ::SelectObject(dc_, oldP);
  ::DeleteObject(b);
  ::DeleteObject(p);
}

void Painter::StrokeRound(const RECT& rc, COLORREF color, int radius, int width) const {
  HPEN p = ::CreatePen(PS_SOLID, width, color);
  HBRUSH oldB = static_cast<HBRUSH>(::SelectObject(dc_, ::GetStockObject(NULL_BRUSH)));
  HPEN oldP = static_cast<HPEN>(::SelectObject(dc_, p));
  // GDI 描边以路径为中心，内缩半像素避免右侧/底边被裁掉
  ::RoundRect(dc_, rc.left, rc.top, rc.right - 1, rc.bottom - 1, radius, radius);
  ::SelectObject(dc_, oldB);
  ::SelectObject(dc_, oldP);
  ::DeleteObject(p);
}

void Painter::GradientV(const RECT& rc, COLORREF top, COLORREF bottom) const {
  GradientFillV(dc_, rc, top, bottom);
}

void Painter::GradientRound(const RECT& rc, COLORREF top, COLORREF bottom, int radius) const {
  HRGN rgn = ::CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius, radius);
  ::SelectClipRgn(dc_, rgn);
  GradientFillV(dc_, rc, top, bottom);
  ::SelectClipRgn(dc_, nullptr);
  ::DeleteObject(rgn);
}

void Painter::HLine(int left, int right, int y, COLORREF color) const {
  RECT rc{left, y, right, y + 1};
  Fill(rc, color);
}

void Painter::VLine(int top, int bottom, int x, COLORREF color) const {
  RECT rc{x, top, x + 1, bottom};
  Fill(rc, color);
}

void Painter::TextLeft(const RECT& rc, const std::wstring& text, COLORREF color,
                       HFONT font) const {
  HFONT old = static_cast<HFONT>(::SelectObject(dc_, font ? font : fonts.ui));
  ::SetTextColor(dc_, color);
  RECT r = rc;
  ::DrawTextW(dc_, text.c_str(), static_cast<int>(text.size()), &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SelectObject(dc_, old);
}

void Painter::TextCenter(const RECT& rc, const std::wstring& text, COLORREF color,
                         HFONT font) const {
  HFONT old = static_cast<HFONT>(::SelectObject(dc_, font ? font : fonts.ui));
  ::SetTextColor(dc_, color);
  RECT r = rc;
  ::DrawTextW(dc_, text.c_str(), static_cast<int>(text.size()), &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SelectObject(dc_, old);
}

void Painter::TextRight(const RECT& rc, const std::wstring& text, COLORREF color,
                        HFONT font) const {
  HFONT old = static_cast<HFONT>(::SelectObject(dc_, font ? font : fonts.ui));
  ::SetTextColor(dc_, color);
  RECT r = rc;
  ::DrawTextW(dc_, text.c_str(), static_cast<int>(text.size()), &r,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SelectObject(dc_, old);
}

void Painter::TextEllipsis(const RECT& rc, const std::wstring& text, COLORREF color,
                           HFONT font) const {
  HFONT old = static_cast<HFONT>(::SelectObject(dc_, font ? font : fonts.ui));
  const int oldBk = ::SetBkMode(dc_, TRANSPARENT);  // 不透明底色会盖住行底色
  ::SetTextColor(dc_, color);
  RECT r = rc;
  ::DrawTextW(dc_, text.c_str(), static_cast<int>(text.size()), &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  ::SetBkMode(dc_, oldBk);
  ::SelectObject(dc_, old);
}

int Painter::TextWidth(const std::wstring& text, HFONT font) const {
  HFONT old = static_cast<HFONT>(::SelectObject(dc_, font ? font : fonts.ui));
  SIZE sz{};
  ::GetTextExtentPoint32W(dc_, text.c_str(), static_cast<int>(text.size()), &sz);
  ::SelectObject(dc_, old);
  return sz.cx;
}

int Painter::TextHeight(HFONT font) const {
  HFONT old = static_cast<HFONT>(::SelectObject(dc_, font ? font : fonts.ui));
  TEXTMETRICW tm{};
  ::GetTextMetricsW(dc_, &tm);
  ::SelectObject(dc_, old);
  return tm.tmHeight;
}

void Painter::Icon(IconId id, const RECT& rc, COLORREF color, bool enabled) const {
  DrawGlyph(dc_, id, rc, color, enabled);
}

void Painter::BitmapIcon(HICON icon, const RECT& rc) const {
  if (!icon) return;
  ::DrawIconEx(dc_, rc.left, rc.top, icon, rc.right - rc.left, rc.bottom - rc.top, 0, nullptr,
               DI_NORMAL);
}

// ───────────────────────── Canvas ─────────────────────────

Canvas::Canvas(HDC target, const RECT& rc) : target_(target), rc_(rc), painter_(nullptr) {
  mem_ = ::CreateCompatibleDC(target);
  bmp_ = ::CreateCompatibleBitmap(target, rc.right - rc.left, rc.bottom - rc.top);
  if (bmp_) oldBmp_ = static_cast<HBITMAP>(::SelectObject(mem_, bmp_));
  ::SetBkMode(mem_, TRANSPARENT);
  // 允许调用方继续用客户区绝对坐标绘制，位图内部只装 rc 这一块
  if (rc.left || rc.top) ::SetViewportOrgEx(mem_, -rc.left, -rc.top, nullptr);
  painter_ = Painter(mem_);
}

Canvas::~Canvas() {
  if (bmp_) {
    ::SelectObject(mem_, oldBmp_);
    ::DeleteObject(bmp_);
  }
  if (mem_) ::DeleteDC(mem_);
}

void Canvas::Commit() {
  // BitBlt 的源坐标同样受视口原点影响，贴回前先还原
  ::SetViewportOrgEx(mem_, 0, 0, nullptr);
  ::BitBlt(target_, rc_.left, rc_.top, rc_.right - rc_.left, rc_.bottom - rc_.top, mem_, 0, 0,
           SRCCOPY);
}

}  // namespace AUI
