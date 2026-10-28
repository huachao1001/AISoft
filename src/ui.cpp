#include "ui.h"

#include "util.h"

namespace ui {

Fonts fonts;

void Fonts::Create(int dpi) {
  Destroy();
  auto make = [&](int pt, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -::MulDiv(pt, dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    return ::CreateFontIndirectW(&lf);
  };
  ui = make(9, FW_NORMAL);
  uiBold = make(9, FW_SEMIBOLD);
}

void Fonts::Destroy() {
  for (HFONT* f : {&ui, &uiBold}) {
    if (*f) {
      ::DeleteObject(*f);
      *f = nullptr;
    }
  }
}

// ───────────────────────── MemDC ─────────────────────────

MemDC::MemDC(HDC target, const RECT& rc) : target_(target), rc_(rc) {
  mem_ = ::CreateCompatibleDC(target);
  bmp_ = ::CreateCompatibleBitmap(target, rc.right - rc.left, rc.bottom - rc.top);
  if (bmp_) oldBmp_ = static_cast<HBITMAP>(::SelectObject(mem_, bmp_));
  ::SetBkMode(mem_, TRANSPARENT);
}

MemDC::~MemDC() {
  if (bmp_) {
    ::SelectObject(mem_, oldBmp_);
    ::DeleteObject(bmp_);
  }
  if (mem_) ::DeleteDC(mem_);
}

void MemDC::Commit() {
  ::BitBlt(target_, rc_.left, rc_.top, rc_.right - rc_.left, rc_.bottom - rc_.top, mem_, 0, 0,
           SRCCOPY);
}

// ───────────────────────── 基础绘制 ─────────────────────────

void FillRect(HDC dc, const RECT& rc, COLORREF color) {
  HBRUSH b = ::CreateSolidBrush(color);
  ::FillRect(dc, &rc, b);
  ::DeleteObject(b);
}

// ───────────────────────── 窗口/对话框公共设施 ─────────────────────────

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
    wc.hIcon = static_cast<HICON>(
        ::LoadImageW(inst, MAKEINTRESOURCEW(iconId), IMAGE_ICON, 32, 32, 0));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(inst, MAKEINTRESOURCEW(iconId), IMAGE_ICON,
                                                 ::GetSystemMetrics(SM_CXSMICON),
                                                 ::GetSystemMetrics(SM_CYSMICON), 0));
  }
  // 已注册（例如同名类被重复创建）视为成功
  return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND CreateDlgItem(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x,
                   int y, int w, int h, int id, DWORD exStyle) {
  const int dpi = util::DpiForWindow(parent);
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
  MemDC mem(target, rc);
  FillRect(mem.dc(), rc, kDialogBg);
  mem.Commit();
  ::ReleaseDC(hwnd, target);
}

HBRUSH DialogCtlBrush(UINT msg, HWND ctl) {
  if (msg == WM_CTLCOLORBTN) {
    // 只有分组框/复选/单选需要底色，普通按钮保持系统观感
    const LONG type = ctl ? (::GetWindowLongW(ctl, GWL_STYLE) & BS_TYPEMASK) : 0;
    if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_OWNERDRAW) return nullptr;
  }
  static HBRUSH brush = ::CreateSolidBrush(kDialogBg);
  return brush;
}

RECT DialogRect(HWND owner, int w, int h) {
  RECT out{CW_USEDEFAULT, CW_USEDEFAULT, w, h};
  RECT ref{};
  // 没有 owner 时用工作区居中：右键菜单调起的对话框就是这种情况
  if ((owner && ::GetWindowRect(owner, &ref)) ||
      ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &ref, 0)) {
    out.left = ref.left + ((ref.right - ref.left) - w) / 2;
    out.top = ref.top + ((ref.bottom - ref.top) - h) / 2;
  }
  return out;
}

// 只有本进程自己的窗口才能当模态 owner。外部窗口（桌面、任务栏、别的程序）
// 一旦被 EnableWindow(FALSE)，程序异常退出就永久锁死用户界面，绝不接受。
bool IsOwnWindow(HWND h) {
  if (!h || !::IsWindow(h)) return false;
  DWORD pid = 0;
  ::GetWindowThreadProcessId(h, &pid);
  return pid == ::GetCurrentProcessId();
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

}  // namespace ui
