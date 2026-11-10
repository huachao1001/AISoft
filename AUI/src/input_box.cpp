#include "AUI/input_box.h"

#include "AUI/font.h"
#include "AUI/painter.h"
#include "AUI/theme.h"

#include <imm.h>
#include <windowsx.h>

#include <algorithm>

namespace AUI {
namespace {

// 光标闪烁周期（毫秒），与系统默认观感接近
constexpr UINT kBlinkMs = 530;
// 光标最小可视宽度
constexpr int kCaretW = 1;

// 单行文本的等宽/比例混排不适合自己算，统一用 GDI 取宽
int MeasureWidth(HDC dc, HFONT font, const std::wstring& s, int from, int len) {
  if (len <= 0) return 0;
  HFONT old = static_cast<HFONT>(::SelectObject(dc, font));
  SIZE sz{};
  ::GetTextExtentPoint32W(dc, s.c_str() + from, len, &sz);
  ::SelectObject(dc, old);
  return sz.cx;
}

}  // namespace

void InputBox::SetSpec(const InputSpec& spec) {
  spec_ = spec;
  ScrollCaretIntoView();
  Invalidate(true);
}

void InputBox::SetCue(const std::wstring& cue) {
  spec_.cue = cue;
  Invalidate(true);
}

void InputBox::SetText(const std::wstring& text) {
  text_ = text;
  caret_ = static_cast<int>(text_.size());
  selBegin_ = selEnd_ = caret_;
  scrollX_ = 0;
  ClampCaret();
  ScrollCaretIntoView();
  ResetBlink();
  Invalidate(true);
}

void InputBox::SelectAll() {
  selBegin_ = 0;
  selEnd_ = static_cast<int>(text_.size());
  caret_ = selEnd_;
  ResetBlink();
  Invalidate(true);
}

int InputBox::TextLeft() const {
  return px(spec_.padX) + (spec_.hasLeadIcon ? px(spec_.leadIconSize + spec_.leadIconGap) : 0);
}

RECT InputBox::TextRect() const {
  const RECT rc = ClientRect();
  HDC dc = ::GetDC(hwnd_);
  const int lineH = (std::max)(px(16), Painter(dc).TextHeight(fonts.ui));
  ::ReleaseDC(hwnd_, dc);

  const int top = rc.top + (rc.bottom - rc.top - lineH) / 2;
  // 文本区左右各留 padX，右侧再留 2px 防止光标贴边被圆角切掉
  return RECT{TextLeft() - scrollX_, top, rc.right - px(spec_.padX) - px(2), top + lineH};
}

int InputBox::XForPos(int pos) const {
  const int clamped = std::clamp(pos, 0, static_cast<int>(text_.size()));
  HDC dc = ::GetDC(hwnd_);
  const int w = MeasureWidth(dc, fonts.ui, text_, 0, clamped);
  ::ReleaseDC(hwnd_, dc);
  return TextLeft() + w - scrollX_;
}

int InputBox::HitTest(int x) const {
  HDC dc = ::GetDC(hwnd_);
  HFONT old = static_cast<HFONT>(::SelectObject(dc, fonts.ui));
  const int target = x - TextLeft() + scrollX_;
  int best = 0;
  int bestDiff = INT_MAX;
  const int n = static_cast<int>(text_.size());
  for (int i = 0; i <= n; ++i) {
    SIZE sz{};
    if (i > 0) ::GetTextExtentPoint32W(dc, text_.c_str(), i, &sz);
    const int diff = std::abs(sz.cx - target);
    if (diff < bestDiff) {
      bestDiff = diff;
      best = i;
    }
  }
  ::SelectObject(dc, old);
  ::ReleaseDC(hwnd_, dc);
  return best;
}

void InputBox::ClampCaret() {
  const int n = static_cast<int>(text_.size());
  caret_ = std::clamp(caret_, 0, n);
  selBegin_ = std::clamp(selBegin_, 0, n);
  selEnd_ = std::clamp(selEnd_, 0, n);
}

void InputBox::ScrollCaretIntoView() {
  EnsureCaretVisible();
}

void InputBox::EnsureCaretVisible() {
  const RECT rc = ClientRect();
  const int avail = rc.right - px(spec_.padX) - px(2) - TextLeft();
  HDC dc = ::GetDC(hwnd_);
  const int caretW = MeasureWidth(dc, fonts.ui, text_, 0, caret_);
  ::ReleaseDC(hwnd_, dc);

  if (caretW - scrollX_ < 0) {
    scrollX_ = caretW;
  } else if (caretW - scrollX_ > avail) {
    scrollX_ = caretW - avail;
  }
  scrollX_ = (std::max)(0, scrollX_);
}

void InputBox::ResetBlink() {
  caretOn_ = true;
  if (blinkTimer_) ::KillTimer(hwnd_, blinkTimer_);
  blinkTimer_ = ::SetTimer(hwnd_, 1, kBlinkMs, nullptr);
}

void InputBox::DeleteSelection() {
  if (!HasSelection()) return;
  const int lo = SelLow();
  const int hi = SelHigh();
  text_.erase(lo, hi - lo);
  caret_ = lo;
  selBegin_ = selEnd_ = lo;
  ClampCaret();
}

void InputBox::InsertText(const std::wstring& s) {
  if (s.empty()) return;
  DeleteSelection();
  text_.insert(caret_, s);
  caret_ += static_cast<int>(s.size());
  selBegin_ = selEnd_ = caret_;
  ClampCaret();
  EnsureCaretVisible();
  ResetBlink();
  Invalidate();
  if (onChange_) onChange_();
}

void InputBox::MoveCaret(int pos, bool extend) {
  const int n = static_cast<int>(text_.size());
  caret_ = std::clamp(pos, 0, n);
  if (extend) {
    selEnd_ = caret_;
  } else {
    selBegin_ = selEnd_ = caret_;
  }
  EnsureCaretVisible();
  ResetBlink();
  Invalidate();
}

void InputBox::MoveWord(bool right, bool extend) {
  int pos = caret_;
  const int n = static_cast<int>(text_.size());
  auto isWord = [](wchar_t c) { return ::iswalnum(c) || c == L'_' || c == L'.' || c == L'-'; };
  if (right) {
    while (pos < n && isWord(text_[pos])) ++pos;
    while (pos < n && !isWord(text_[pos])) ++pos;
  } else {
    while (pos > 0 && !isWord(text_[pos - 1])) --pos;
    while (pos > 0 && isWord(text_[pos - 1])) --pos;
  }
  MoveCaret(pos, extend);
}

void InputBox::MoveLineEdge(bool right, bool extend) {
  MoveCaret(right ? static_cast<int>(text_.size()) : 0, extend);
}

void InputBox::CopyToClipboard() const {
  if (!HasSelection()) return;
  const std::wstring part = text_.substr(SelLow(), SelHigh() - SelLow());
  if (!::OpenClipboard(hwnd_)) return;
  ::EmptyClipboard();
  const size_t bytes = (part.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* dst = ::GlobalLock(mem)) {
      memcpy(dst, part.c_str(), bytes);
      ::GlobalUnlock(mem);
      ::SetClipboardData(CF_UNICODETEXT, mem);
    }
  }
  ::CloseClipboard();
}

void InputBox::CutToClipboard() {
  CopyToClipboard();
  DeleteSelection();
  EnsureCaretVisible();
  Invalidate();
  if (onChange_) onChange_();
}

void InputBox::PasteFromClipboard() {
  if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
  if (!::OpenClipboard(hwnd_)) return;
  std::wstring data;
  if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
    if (const auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
      data = p;
      ::GlobalUnlock(h);
    }
  }
  ::CloseClipboard();
  // 单行输入框：把换行折成空格
  for (auto& c : data) {
    if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
  }
  InsertText(data);
}

void InputBox::OnSize(int cx, int cy) {
  (void)cx;
  (void)cy;
  EnsureCaretVisible();
  Invalidate();
}

void InputBox::OnPaint() {
  const RECT rc = ClientRect();
  if (rc.right <= 0 || rc.bottom <= 0) return;
  const int radius = px(spec_.radius);

  HDC target = ::GetDC(hwnd_);
  Canvas canvas(target, rc);
  Painter& p = canvas.painter();

  p.Fill(rc, spec_.outerBg);
  p.FillRound(rc, kInputBg, radius);
  p.StrokeRound(rc, focused_ ? kInputFocusBorder : kInputBorder, radius, 1);

  const RECT tr = TextRect();
  int textLeft = tr.left;
  if (spec_.hasLeadIcon) {
    const int is = px(spec_.leadIconSize);
    RECT ir{px(spec_.padX), (rc.top + rc.bottom - is) / 2, px(spec_.padX) + is,
            (rc.top + rc.bottom - is) / 2 + is};
    p.Icon(spec_.leadIcon, ir, kTextFaint);
    textLeft = ir.right + px(spec_.leadIconGap) - scrollX_;
  }

  // 文本裁剪到输入区，避免滚动时溢出到圆角外
  const HRGN clip = ::CreateRectRgn(px(spec_.padX), rc.top, rc.right - px(spec_.padX), rc.bottom);
  ::SelectClipRgn(canvas.dc(), clip);

  const std::wstring shown = text_;
  if (shown.empty() && composition_.empty()) {
    p.TextEllipsis(tr, spec_.cue, kTextFaint, fonts.ui);
  } else {
    // 选区底色
    if (HasSelection()) {
      const int x1 = XForPos(SelLow());
      const int x2 = XForPos(SelHigh());
      RECT sr{x1, tr.top, x2, tr.bottom};
      p.Fill(sr, kSelBg);
    }

    if (!shown.empty()) {
      p.TextLeft(RECT{textLeft, tr.top, tr.right, tr.bottom}, shown, kText, fonts.ui);
    }

    // IME 组合串：画在光标处，带下划线
    if (!composition_.empty()) {
      const int cx = XForPos(caret_);
      RECT cr{cx, tr.top, tr.right, tr.bottom};
      p.TextLeft(cr, composition_, kText, fonts.ui);
      const int w = MeasureWidth(canvas.dc(), fonts.ui, composition_, 0,
                                 static_cast<int>(composition_.size()));
      p.HLine(cx, cx + w, tr.bottom - px(3), kText);
    }

    // 光标
    if (focused_ && caretOn_ && composition_.empty()) {
      const int cx = XForPos(caret_);
      p.Fill(RECT{cx, tr.top, cx + (std::max)(px(kCaretW), 1), tr.bottom}, kText);
    }
  }

  ::SelectClipRgn(canvas.dc(), nullptr);
  ::DeleteObject(clip);
  canvas.Commit();
  ::ReleaseDC(hwnd_, target);
}

bool InputBox::OnEraseBkgnd(HDC dc) {
  RECT rc;
  ::GetClientRect(hwnd_, &rc);
  Painter(dc).Fill(rc, spec_.outerBg);
  return true;
}

LRESULT InputBox::OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_SETFOCUS:
      focused_ = true;
      ResetBlink();
      Invalidate();
      return 0;

    case WM_KILLFOCUS:
      focused_ = false;
      dragging_ = false;
      if (blinkTimer_) {
        ::KillTimer(hwnd_, blinkTimer_);
        blinkTimer_ = 0;
      }
      // 失焦时把组合串落盘，避免残留
      OnCompositionDone();
      Invalidate();
      if (onBlur_) onBlur_();
      return 0;

    case WM_TIMER:
      if (wParam == 1) {
        caretOn_ = !caretOn_;
        Invalidate();
        return 0;
      }
      break;

    case WM_LBUTTONDOWN: {
      ::SetFocus(hwnd_);
      const int x = GET_X_LPARAM(lParam);
      const int pos = HitTest(x);
      const bool shift = (::GetKeyState(VK_SHIFT) < 0);
      dragging_ = true;
      ::SetCapture(hwnd_);
      MoveCaret(pos, shift);
      return 0;
    }

    case WM_LBUTTONDBLCLK: {
      SelectAll();
      return 0;
    }

    case WM_LBUTTONUP:
      if (dragging_) {
        dragging_ = false;
        ::ReleaseCapture();
      }
      return 0;

    case WM_MOUSEMOVE:
      if (dragging_) {
        MoveCaret(HitTest(GET_X_LPARAM(lParam)), true);
        return 0;
      }
      return 0;

    case WM_CHAR: {
      const wchar_t ch = static_cast<wchar_t>(wParam);
      if (ch == L'\r' || ch == L'\n' || ch == L'\t') return 0;
      if (ch == L'\b') {
        if (HasSelection()) {
          DeleteSelection();
        } else if (caret_ > 0) {
          text_.erase(caret_ - 1, 1);
          --caret_;
          selBegin_ = selEnd_ = caret_;
        }
        EnsureCaretVisible();
        ResetBlink();
        Invalidate();
        if (onChange_) onChange_();
        return 0;
      }
      if (ch < L' ' ) return 0;
      InsertText(std::wstring(1, ch));
      return 0;
    }

    case WM_KEYDOWN: {
      const bool ctrl = (::GetKeyState(VK_CONTROL) < 0);
      const bool shift = (::GetKeyState(VK_SHIFT) < 0);

      switch (wParam) {
        case VK_HOME: MoveLineEdge(false, shift); return 0;
        case VK_END: MoveLineEdge(true, shift); return 0;
        case VK_LEFT:
          if (ctrl) {
            MoveWord(false, shift);
          } else {
            MoveCaret(HasSelection() && !shift ? SelLow() : caret_ - 1, shift);
          }
          return 0;
        case VK_RIGHT:
          if (ctrl) {
            MoveWord(true, shift);
          } else {
            MoveCaret(HasSelection() && !shift ? SelHigh() : caret_ + 1, shift);
          }
          return 0;
        case VK_BACK:
          if (HasSelection()) {
            DeleteSelection();
          } else if (caret_ > 0) {
            text_.erase(caret_ - 1, 1);
            --caret_;
            selBegin_ = selEnd_ = caret_;
          }
          EnsureCaretVisible();
          ResetBlink();
          Invalidate();
          if (onChange_) onChange_();
          return 0;
        case VK_DELETE:
          if (HasSelection()) {
            DeleteSelection();
          } else if (caret_ < static_cast<int>(text_.size())) {
            text_.erase(caret_, 1);
          }
          selBegin_ = selEnd_ = caret_;
          EnsureCaretVisible();
          ResetBlink();
          Invalidate();
          if (onChange_) onChange_();
          return 0;
        case VK_RETURN:
          if (onSubmit_) onSubmit_(true);
          return 0;
        case VK_ESCAPE:
          if (onSubmit_) onSubmit_(false);
          return 0;
        case 'A':
          if (ctrl) {
            SelectAll();
            return 0;
          }
          break;
        case 'C':
          if (ctrl) {
            CopyToClipboard();
            return 0;
          }
          break;
        case 'X':
          if (ctrl) {
            CutToClipboard();
            return 0;
          }
          break;
        case 'V':
          if (ctrl) {
            PasteFromClipboard();
            return 0;
          }
          break;
        default:
          break;
      }
      return 0;
    }

    // IME 组合：先记录，上屏后再并入文本
    case WM_IME_STARTCOMPOSITION:
      composition_.clear();
      Invalidate();
      return 0;

    case WM_IME_COMPOSITION: {
      if (lParam & GCS_COMPSTR) {
        HIMC imc = ::ImmGetContext(hwnd_);
        if (imc) {
          const LONG bytes = ::ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
          if (bytes > 0) {
            composition_.resize(static_cast<size_t>(bytes) / sizeof(wchar_t));
            ::ImmGetCompositionStringW(imc, GCS_COMPSTR, composition_.data(), bytes);
          } else {
            composition_.clear();
          }
          ::ImmReleaseContext(hwnd_, imc);
        }
        Invalidate();
      }
      if (lParam & GCS_RESULTSTR) {
        HIMC imc = ::ImmGetContext(hwnd_);
        if (imc) {
          const LONG bytes = ::ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
          if (bytes > 0) {
            std::wstring result(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
            ::ImmGetCompositionStringW(imc, GCS_RESULTSTR, result.data(), bytes);
            ::ImmReleaseContext(hwnd_, imc);
            composition_.clear();
            InsertText(result);
            return 0;
          }
          ::ImmReleaseContext(hwnd_, imc);
        }
        composition_.clear();
        Invalidate();
      }
      return 0;
    }

    case WM_IME_ENDCOMPOSITION:
      OnCompositionDone();
      return 0;

    case WM_GETDLGCODE:
      return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;

    // 自绘窗口没有原生文本存储，补上标准文本消息，便于外部读写
    case WM_SETTEXT: {
      const auto* s = reinterpret_cast<const wchar_t*>(lParam);
      SetText(s ? s : L"");
      return TRUE;
    }

    case WM_GETTEXT: {
      if (!lParam || wParam == 0) return 0;
      const size_t cap = static_cast<size_t>(wParam);
      const size_t n = (std::min)(text_.size(), cap - 1);
      memcpy(reinterpret_cast<void*>(lParam), text_.c_str(), n * sizeof(wchar_t));
      reinterpret_cast<wchar_t*>(lParam)[n] = L'\0';
      return static_cast<LRESULT>(n);
    }

    case WM_GETTEXTLENGTH:
      return static_cast<LRESULT>(text_.size());

    default:
      break;
  }
  return Widget::OnMessage(msg, wParam, lParam);
}

void InputBox::OnCompositionDone() {
  if (composition_.empty()) return;
  const std::wstring pending = composition_;
  composition_.clear();
  InsertText(pending);
}

}  // namespace AUI
