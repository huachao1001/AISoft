#include "AUI/status_bar.h"

#include "AUI/font.h"
#include "AUI/painter.h"

namespace AUI {

void StatusBar::SetFields(const StatusFields& f) {
  fields_ = f;
  Invalidate();
}

void StatusBar::SetStyle(const StatusStyle& style) {
  style_ = style;
  Invalidate();
}

void StatusBar::OnPaint() {
  const RECT rc = ClientRect();
  HDC target = ::GetDC(hwnd_);
  Canvas canvas(target, rc);
  Painter& p = canvas.painter();
  p.Fill(rc, style_.bg);

  const int padX = px(style_.padX);
  const int cy = (rc.top + rc.bottom) / 2;
  int x = padX;

  // 右侧统计先算宽度，左侧被挤时用它做边界
  const int rightW = p.TextWidth(fields_.right, fonts.uiSmall);
  const int rightLeft = rc.right - padX - rightW;
  const int limit = rightLeft - px(16);

  if (fields_.showHome) {
    const int is = px(style_.homeIconSize);
    RECT ir{x, cy - is / 2, x + is, cy + is / 2};
    p.Icon(IconId::kHome, ir, kTagInk);
    x = ir.right + px(8);
  }

  // 路径：先按可用宽度收窄，再画（长度测量不能超过 limit，否则后面全部错位）
  RECT tr{x, rc.top, limit, rc.bottom};
  std::wstring shown = fields_.left;
  while (!shown.empty() && p.TextWidth(shown, fonts.uiSmall) > (limit - x)) {
    shown.erase(shown.begin());
  }
  p.TextEllipsis(tr, shown, style_.text, fonts.uiSmall);
  x += p.TextWidth(shown, fonts.uiSmall);
  if (x > limit) x = limit;

  if (!fields_.tag.empty()) {
    const int tagPadX = px(style_.tagPadX);
    const int tagW = p.TextWidth(fields_.tag, fonts.uiSmall) + tagPadX * 2;
    const int tagH = px(style_.tagH);
    RECT tagRc{x + px(8), cy - tagH / 2, x + px(8) + tagW, cy + tagH / 2};
    if (tagRc.right < limit) {
      p.FillRound(tagRc, kTagBg, px(style_.tagRadius));
      p.TextCenter(tagRc, fields_.tag, kTagInk, fonts.uiSmall);
      x = tagRc.right + px(8);
    }
  }

  if (!fields_.size.empty() && x < limit) {
    RECT sr{x, rc.top, limit, rc.bottom};
    p.TextLeft(sr, fields_.size, style_.text, fonts.uiSmall);
  }

  if (rightLeft > 0) {
    RECT rr{rightLeft, rc.top, rc.right - padX, rc.bottom};
    p.TextEllipsis(rr, fields_.right, style_.text, fonts.uiSmall);
  }

  p.HLine(0, rc.right, 0, style_.divider);
  canvas.Commit();
  ::ReleaseDC(hwnd_, target);
}
bool StatusBar::OnEraseBkgnd(HDC dc) {
  RECT rc;
  ::GetClientRect(hwnd_, &rc);
  Painter(dc).Fill(rc, style_.bg);
  return true;
}

LRESULT StatusBar::OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_SETCURSOR) {
    ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
    return TRUE;
  }
  return Widget::OnMessage(msg, wParam, lParam);
}

}  // namespace AUI
