// 圆角搜索框：自绘边框 + 内嵌无边框 EDIT
#include "ui_search.h"

#include "ui.h"
#include "util.h"

#include <windowsx.h>

#include <map>

namespace ui {
namespace {

constexpr const wchar_t* kSearchClass = L"AIZipSearchBox";
constexpr int kRadius = 7;
constexpr int kPadX = 11;

struct Host {
  HWND edit = nullptr;
  std::wstring cue;
  bool focused = false;
  std::function<void()> onChange;
};

std::map<HWND, Host> g_hosts;

Host* Get(HWND h) {
  auto it = g_hosts.find(h);
  return it == g_hosts.end() ? nullptr : &it->second;
}

RECT EditRect(HWND host) {
  RECT rc;
  ::GetClientRect(host, &rc);
  const int dpi = util::DpiForWindow(host);
  auto px = [dpi](int v) { return ::MulDiv(v, dpi, 96); };
  return RECT{rc.left + px(kPadX), rc.top + px(5), rc.right - px(kPadX), rc.bottom - px(5)};
}

void Reposition(HWND host) {
  Host* h = Get(host);
  if (!h || !h->edit) return;
  const RECT r = EditRect(host);
  ::MoveWindow(h->edit, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
}

bool EditEmpty(HWND edit) { return edit && ::GetWindowTextLengthW(edit) == 0; }

// 空且未聚焦时把 EDIT 藏起来，让宿主自己画提示文字；
// 否则内嵌 EDIT 的白底会盖住提示。
void SyncEditVisible(HWND host) {
  Host* h = Get(host);
  if (!h || !h->edit) return;
  const bool show = h->focused || !EditEmpty(h->edit);
  ::ShowWindow(h->edit, show ? SW_SHOW : SW_HIDE);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                          DWORD_PTR ref) {
  HWND host = reinterpret_cast<HWND>(ref);
  switch (msg) {
    case WM_GETDLGCODE:
      // IsDialogMessage 会吞回车，明确要全部按键
      return DLGC_WANTALLKEYS | DLGC_WANTMESSAGE;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
      if (Host* h = Get(host)) {
        h->focused = (msg == WM_SETFOCUS);
        SyncEditVisible(host);
        ::InvalidateRect(host, nullptr, TRUE);
      }
      break;
    }
    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) {
        ::SetWindowTextW(hwnd, L"");
        if (Host* h = Get(host)) {
          SyncEditVisible(host);
          if (h->onChange) h->onChange();
        }
        return 0;
      }
      break;
    case WM_CHAR:
      // 回车不插入换行，交给列表处理
      if (wParam == VK_RETURN) return 0;
      break;
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, EditProc, id);
      break;
    default:
      break;
  }
  return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SearchProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_SIZE:
      Reposition(hwnd);
      return 0;

    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      FillRect(dc, rc, kPaneBg);
      return 1;
    }

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = ::BeginPaint(hwnd, &ps);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      const int dpi = util::DpiForWindow(hwnd);
      const int radius = ::MulDiv(kRadius, dpi, 96);
      Host* h = Get(hwnd);
      const bool focused = h && h->focused;

      FillRoundRect(dc, rc, kInputBg, radius);
      StrokeRoundRect(dc, rc, focused ? kInputFocusBorder : kInputBorder, radius, 1);

      if (h && EditEmpty(h->edit)) {
        RECT tr = rc;
        tr.left += ::MulDiv(kPadX, dpi, 96);
        tr.right -= ::MulDiv(kPadX, dpi, 96);
        DrawTextEllipsis(dc, tr, h->cue, kTextFaint, fonts.ui);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_COMMAND: {
      if (HIWORD(wParam) == EN_CHANGE) {
        if (Host* h = Get(hwnd)) {
          SyncEditVisible(hwnd);
          if (h->onChange) h->onChange();
        }
      }
      return 0;
    }

    case WM_LBUTTONDOWN:
      if (Host* h = Get(hwnd); h && h->edit) {
        ::ShowWindow(h->edit, SW_SHOW);
        ::SetFocus(h->edit);
      }
      return 0;

    case WM_SETFOCUS:
      // Tab / 点击进入时焦点直接落到内嵌 EDIT
      if (Host* h = Get(hwnd); h && h->edit) {
        ::ShowWindow(h->edit, SW_SHOW);
        ::SetFocus(h->edit);
      }
      return 0;

    case WM_NCDESTROY:
      g_hosts.erase(hwnd);
      break;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

HWND CreateSearchBox(HWND parent, HINSTANCE inst, int id, const std::wstring& cue) {
  static bool registered = false;
  if (!registered) {
    registered = RegisterWindowClass(kSearchClass, SearchProc, inst, CS_HREDRAW | CS_VREDRAW,
                                     nullptr);
  }
  HWND host = ::CreateWindowExW(0, kSearchClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
  Host& h = g_hosts[host];
  h.cue = cue;

  h.edit = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0,
                             host,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id + 1)), inst, nullptr);
  ::SendMessageW(h.edit, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui), TRUE);
  ::SendMessageW(h.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
  ::SetWindowSubclass(h.edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(host));
  return host;
}

std::wstring SearchBoxText(HWND box) {
  Host* h = Get(box);
  if (!h || !h->edit) return L"";
  wchar_t buf[512] = {};
  ::GetWindowTextW(h->edit, buf, 512);
  return buf;
}

void SetSearchBoxCue(HWND box, const std::wstring& cue) {
  if (Host* h = Get(box)) {
    h->cue = cue;
    ::InvalidateRect(box, nullptr, TRUE);
  }
}

void SetSearchBoxChangeHandler(HWND box, const std::function<void()>& fn) {
  if (Host* h = Get(box)) h->onChange = fn;
}

HWND SearchBoxEdit(HWND box) {
  Host* h = Get(box);
  return h ? h->edit : nullptr;
}

}  // namespace ui
