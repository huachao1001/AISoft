// 对话框公共设施实现
#include "dlg_util.h"

#include "ui_theme.h"

#include <objidl.h>
#include <gdiplus.h>

#include <string>

namespace ui {
namespace {

// 只有本进程的窗口才能当模态 owner，否则异常退出会锁死外部界面
bool IsOwnWindow(HWND hwnd) {
  if (!hwnd || !::IsWindow(hwnd)) return false;
  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  return pid == ::GetCurrentProcessId();
}

}  // namespace

HFONT DialogFont() {
  static HFONT font = [] {
    HDC dc = ::GetDC(nullptr);
    const int dpi = ::GetDeviceCaps(dc, LOGPIXELSY);
    ::ReleaseDC(nullptr, dc);

    LOGFONTW lf{};
    lf.lfHeight = -::MulDiv(9, dpi > 0 ? dpi : 96, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    return ::CreateFontIndirectW(&lf);
  }();
  return font;
}

bool RegisterWindowClass(const wchar_t* name, WNDPROC proc, HINSTANCE instance, UINT style,
                         HBRUSH background, int iconId) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = style;
  wc.lpfnWndProc = proc;
  wc.hInstance = instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = background;
  wc.lpszClassName = name;
  if (iconId) {
    wc.hIcon = static_cast<HICON>(
        ::LoadImageW(instance, MAKEINTRESOURCEW(iconId), IMAGE_ICON, 32, 32, 0));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(iconId), IMAGE_ICON,
                                                 ::GetSystemMetrics(SM_CXSMICON),
                                                 ::GetSystemMetrics(SM_CYSMICON), 0));
  }
  return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

RECT DialogRect(HWND owner, int width, int height) {
  RECT out{CW_USEDEFAULT, CW_USEDEFAULT, width, height};
  RECT reference{};
  if ((owner && ::GetWindowRect(owner, &reference)) ||
      ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &reference, 0)) {
    out.left = reference.left + ((reference.right - reference.left) - width) / 2;
    out.top = reference.top + ((reference.bottom - reference.top) - height) / 2;
  }
  return out;
}

void RunModalDialog(HWND dialog, HWND owner, const std::function<void()>& onBeforeEnable) {
  if (!IsOwnWindow(owner)) owner = nullptr;
  if (owner) ::EnableWindow(owner, FALSE);

  MSG msg;
  while (::IsWindow(dialog) && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!::IsDialogMessageW(dialog, &msg)) {
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

HWND CreateDlgItem(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style, int x,
                   int y, int w, int h, int id, DWORD exStyle) {
  HDC dc = ::GetDC(parent);
  const int dpi = dc ? ::GetDeviceCaps(dc, LOGPIXELSX) : 96;
  if (dc) ::ReleaseDC(parent, dc);
  auto px = [dpi](int v) { return ::MulDiv(v, dpi > 0 ? dpi : 96, 96); };

  HWND child = ::CreateWindowExW(exStyle, className, text, WS_CHILD | WS_VISIBLE | style, px(x),
                                 px(y), px(w), px(h), parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 reinterpret_cast<HINSTANCE>(
                                     ::GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                                 nullptr);
  if (child) ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(DialogFont()), TRUE);
  return child;
}

void PaintDialogBackground(HWND hwnd) {
  RECT rc;
  ::GetClientRect(hwnd, &rc);

  Gdiplus::Graphics graphics(hwnd);
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
  // 与 GDI 刷子共用同一组分量，避免两处硬编码漂移
  Gdiplus::SolidBrush brush(Gdiplus::Color(kDialogR, kDialogG, kDialogB));
  graphics.FillRectangle(&brush, Gdiplus::Rect(0, 0, rc.right, rc.bottom));}

HBRUSH DialogCtlBrush(UINT msg, HWND control) {
  if (msg == WM_CTLCOLORBTN) {
    const LONG type = control ? (::GetWindowLongW(control, GWL_STYLE) & BS_TYPEMASK) : 0;
    if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_OWNERDRAW) return nullptr;
  }
  static HBRUSH brush = ::CreateSolidBrush(kDialogBg);
  return brush;
}

}  // namespace ui
