#include "AUI/button.h"

#include "AUI/font.h"
#include "AUI/painter.h"

#include <windowsx.h>

namespace AUI {
namespace {

// 主题色的三个表面状态
struct Palette {
  COLORREF bg;
  COLORREF border;
  COLORREF ink;
};

Palette Resolve(const ButtonSpec& s, bool enabled, bool hot, bool pressed) {
  Palette p{};
  if (!enabled) {
    p.bg = (s.style == ButtonStyle::kOutline) ? kBtnDisBg : kUseTheme;
    p.border = kBtnDisBorder;
    p.ink = kTextFaint;
    return p;
  }
  switch (s.style) {
    case ButtonStyle::kOutline:
      p.bg = pressed ? kBtnDownBg : (hot ? kBtnHoverBg : kPaneBg);
      p.border = hot ? kBtnHoverBorder : kBtnBorder;
      p.ink = kInk;
      break;
    case ButtonStyle::kSubtle:
      p.bg = pressed ? kBtnDownBg : (hot ? kHoverBg : kUseTheme);
      p.border = kUseTheme;
      p.ink = kInk;
      break;
    case ButtonStyle::kFlat:
      p.bg = pressed ? kBtnDownBg : (hot ? kHoverBg : kUseTheme);
      p.border = kUseTheme;
      p.ink = kInk;
      break;
  }
  if (s.bg != kUseTheme) p.bg = s.bg;
  if (s.border != kUseTheme) p.border = s.border;
  if (s.ink != kUseTheme) p.ink = s.ink;
  return p;
}

}  // namespace

void Button::SetSpec(const ButtonSpec& spec) {
  spec_ = spec;
  Invalidate();
}

void Button::SetLabel(const std::wstring& label) {
  spec_.label = label;
  Invalidate();
}

void Button::SetIcon(IconId icon) {
  spec_.icon = icon;
  spec_.hasIcon = true;
  Invalidate();
}

void Button::SetEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  if (!enabled) {
    hot_ = false;
    pressed_ = false;
  }
  Invalidate();
}

int Button::PreferredWidth() {
  RECT rc{};
  HDC screen = ::GetDC(nullptr);
  Painter p(screen);
  RECT iconRc{}, textRc{};
  bool showIcon = false, showText = false;
  LayoutParts(p, RECT{0, 0, 0, 0}, iconRc, textRc, showIcon, showText);

  int w = px(spec_.padX) * 2;
  if (showIcon) w += px(spec_.iconSize);
  if (showIcon && showText) w += px(spec_.iconGap);
  if (showText) w += p.TextWidth(spec_.label, fonts.ui);
  ::ReleaseDC(nullptr, screen);
  return w;
}

void Button::LayoutParts(Painter& p, const RECT& rc, RECT& iconRc, RECT& textRc, bool& showIcon,
                         bool& showText) {
  showIcon = spec_.hasIcon && spec_.placement != IconPlacement::kNone;
  showText = spec_.hasText && spec_.placement != IconPlacement::kOnly && !spec_.label.empty();
  iconRc = RECT{};
  textRc = RECT{};

  const int iconSz = px(spec_.iconSize);
  const int gap = px(spec_.iconGap);
  const int padX = px(spec_.padX);
  const int textW = showText ? p.TextWidth(spec_.label, fonts.ui) : 0;
  const int contentW = (showIcon ? iconSz : 0) + (showIcon && showText ? gap : 0) + textW;
  const int cy = (rc.top + rc.bottom) / 2;
  int x = rc.left + (rc.right - rc.left - contentW) / 2;

  if (spec_.placement == IconPlacement::kTop) {
    const int textH = showText ? p.TextHeight(fonts.ui) : 0;
    const int gapY = px(4);
    const int totalH = iconSz + (showText ? gapY + textH : 0);
    const int top = rc.top + ((rc.bottom - rc.top) - totalH) / 2;
    iconRc = RECT{x + (contentW - iconSz) / 2, top, x + (contentW - iconSz) / 2 + iconSz,
                  top + iconSz};
    textRc = RECT{rc.left + padX, iconRc.bottom + gapY, rc.right - padX,
                  iconRc.bottom + gapY + textH};
    return;
  }

  if (showIcon) {
    iconRc = RECT{x, cy - iconSz / 2, x + iconSz, cy - iconSz / 2 + iconSz};
    x += iconSz + gap;
  }
  if (showText) textRc = RECT{x, rc.top, rc.right - padX, rc.bottom};
}

void Button::PaintButton(Painter& p, const RECT& rc) {
  const Palette c = Resolve(spec_, enabled_, hot_, pressed_);
  if (c.bg != kUseTheme) {
    if (spec_.style == ButtonStyle::kOutline) {
      p.FillRound(rc, c.bg, px(spec_.radius));
    } else {
      p.FillRound(rc, c.bg, px(spec_.radius));
    }
  }
  if (spec_.style == ButtonStyle::kOutline && c.border != kUseTheme) {
    p.StrokeRound(rc, c.border, px(spec_.radius), 1);
  }

  RECT iconRc{}, textRc{};
  bool showIcon = false, showText = false;
  LayoutParts(p, rc, iconRc, textRc, showIcon, showText);
  if (showIcon) p.Icon(spec_.icon, iconRc, c.ink, enabled_);
  if (showText) p.TextLeft(textRc, spec_.label, c.ink, fonts.ui);
}

void Button::OnSize(int cx, int cy) {
  (void)cx;
  (void)cy;
  Invalidate();
}

void Button::OnPaint() {
  const RECT rc = ClientRect();
  HDC target = ::GetDC(hwnd_);
  Canvas canvas(target, rc);
  Painter& p = canvas.painter();
  p.Fill(rc, kPaneBg);
  PaintButton(p, rc);
  canvas.Commit();
  ::ReleaseDC(hwnd_, target);
}

bool Button::OnEraseBkgnd(HDC dc) {
  RECT rc;
  ::GetClientRect(hwnd_, &rc);
  Painter(dc).Fill(rc, kPaneBg);
  return true;
}

LRESULT Button::OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEMOVE:
      if (!tracking_) {
        tracking_ = true;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        ::TrackMouseEvent(&tme);
      }
      if (!hot_) {
        hot_ = true;
        Invalidate();
      }
      return 0;

    case WM_MOUSELEAVE:
      tracking_ = false;
      hot_ = false;
      Invalidate();
      return 0;

    case WM_LBUTTONDOWN:
      if (enabled_) {
        pressed_ = true;
        ::SetCapture(hwnd_);
        Invalidate();
      }
      return 0;

    case WM_LBUTTONUP: {
      if (!pressed_) return 0;
      pressed_ = false;
      ::ReleaseCapture();
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      RECT rc;
      ::GetClientRect(hwnd_, &rc);
      const bool inside = ::PtInRect(&rc, pt) != 0;
      Invalidate();
      if (inside && enabled_ && handler_) handler_();
      return 0;
    }

    case WM_SETCURSOR:
      ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
      return TRUE;

    default:
      break;
  }
  return Widget::OnMessage(msg, wParam, lParam);
}

}  // namespace AUI
