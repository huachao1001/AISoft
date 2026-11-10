#include "AUI/dialog.h"

#include "AUI/painter.h"
#include "AUI/theme.h"
#include "AUI/platform.h"

namespace AUI {
namespace {

// 只有本进程自己的窗口才能当模态 owner，否则异常退出会锁死外部界面
bool IsOwnWindow(HWND h) {
  if (!h || !::IsWindow(h)) return false;
  DWORD pid = 0;
  ::GetWindowThreadProcessId(h, &pid);
  return pid == ::GetCurrentProcessId();
}

}  // namespace

bool RegisterWindowClass(const wchar_t* name, WNDPROC proc, HINSTANCE inst, UINT style,
                         HBRUSH bg, int iconId) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = style;
  wc.lpfnWndProc = proc;
  wc.hInstance = inst;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = bg;
  wc.lpszClassName = name;
  if (iconId) {
    wc.hIcon =
        static_cast<HICON>(::LoadImageW(inst, MAKEINTRESOURCEW(iconId), IMAGE_ICON, 32, 32, 0));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(inst, MAKEINTRESOURCEW(iconId), IMAGE_ICON,
                                                 ::GetSystemMetrics(SM_CXSMICON),
                                                 ::GetSystemMetrics(SM_CYSMICON), 0));
  }
  return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND CreateDlgItem(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x,
                   int y, int w, int h, int id, DWORD exStyle) {
  const int dpi = DpiForWindow(parent);
  auto px = [dpi](int v) { return ::MulDiv(v, dpi, 96); };
  return ::CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w),
                           px(h), parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                           nullptr);
}

void PaintDialogBackground(HWND hwnd) {
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  HDC target = ::GetDC(hwnd);
  Canvas canvas(target, rc);
  canvas.painter().Fill(rc, kDialogBg);
  canvas.Commit();
  ::ReleaseDC(hwnd, target);
}

HBRUSH DialogCtlBrush(UINT msg, HWND ctl) {
  if (msg == WM_CTLCOLORBTN) {
    const LONG type = ctl ? (::GetWindowLongW(ctl, GWL_STYLE) & BS_TYPEMASK) : 0;
    if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_OWNERDRAW) return nullptr;
  }
  static HBRUSH brush = ::CreateSolidBrush(kDialogBg);
  return brush;
}

RECT DialogRect(HWND owner, int w, int h) {
  RECT out{CW_USEDEFAULT, CW_USEDEFAULT, w, h};
  RECT ref{};
  if ((owner && ::GetWindowRect(owner, &ref)) ||
      ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &ref, 0)) {
    out.left = ref.left + ((ref.right - ref.left) - w) / 2;
    out.top = ref.top + ((ref.bottom - ref.top) - h) / 2;
  }
  return out;
}

void RunModalDialog(HWND dlg, HWND owner, const std::function<void()>& onBeforeEnable) {
  if (!IsOwnWindow(owner)) owner = nullptr;
  if (owner) ::EnableWindow(owner, FALSE);
  MSG msg;
  while (::IsWindow(dlg) && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!::IsDialogMessageW(dlg, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }
  if (onBeforeEnable) onBeforeEnable();
  if (owner) {
    ::EnableWindow(owner, TRUE);
    ::SetActiveWindow(owner);
  }
}

}  // namespace AUI
