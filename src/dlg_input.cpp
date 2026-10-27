// 轻量输入框
#include "dlg_input.h"

#include "app.h"
#include "ui.h"

#include <commctrl.h>

namespace ui {
namespace {

constexpr const wchar_t* kClass = L"MyZipInputDlg";
constexpr int kIdEdit = 4001;
constexpr int kIdOk = IDOK;
constexpr int kIdCancel = IDCANCEL;

struct InputState {
  std::wstring title;
  std::wstring label;
  std::wstring* value = nullptr;
  bool password = false;
  bool accepted = false;
  HWND edit = nullptr;
};

InputState* g_in = nullptr;

void CreateControls(HWND hwnd, HINSTANCE inst) {
  const int dpi = g_app.dpi;
  auto px = [&](int v) { return ::MulDiv(v, dpi, 96); };
  auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
                  int id) {
    return ::CreateWindowExW(WS_EX_CLIENTEDGE, cls, text, WS_CHILD | WS_VISIBLE | style, px(x),
                             px(y), px(w), px(h), hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
  };

  make(L"STATIC", g_in->label.c_str(), SS_LEFT, 14, 14, 316, 18, 0);
  g_in->edit = make(L"EDIT", g_in->value->c_str(),
                    ES_AUTOHSCROLL | (g_in->password ? ES_PASSWORD : 0) | WS_TABSTOP, 14, 36, 316,
                    24, kIdEdit);
  make(L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP, 194, 74, 66, 26, kIdOk);
  make(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 266, 74, 66, 26, kIdCancel);

  ::SendMessageW(g_in->edit, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui), TRUE);
  ::SendMessageW(::GetDlgItem(hwnd, kIdOk), WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui), TRUE);
  ::SendMessageW(::GetDlgItem(hwnd, kIdCancel), WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui),
                 TRUE);
}

void Accept(HWND hwnd) {
  wchar_t buf[1024];
  ::GetWindowTextW(g_in->edit, buf, 1024);
  *g_in->value = buf;
  g_in->accepted = true;
  ::DestroyWindow(hwnd);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      CreateControls(hwnd, reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)));
      return 0;

    case WM_COMMAND:
      if (LOWORD(wParam) == kIdOk) {
        Accept(hwnd);
        return 0;
      }
      if (LOWORD(wParam) == kIdCancel) {
        ::DestroyWindow(hwnd);
        return 0;
      }
      break;

    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool PromptText(HWND owner, const std::wstring& title, const std::wstring& label,
                std::wstring& value, bool password) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Proc;
    wc.hInstance = g_app.instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
    wc.lpszClassName = kClass;
    registered = ::RegisterClassExW(&wc) != 0;
  }

  InputState st;
  st.title = title;
  st.label = label;
  st.value = &value;
  st.password = password;
  g_in = &st;

  const int dpi = g_app.dpi;
  auto px = [&](int v) { return ::MulDiv(v, dpi, 96); };

  RECT ownerRc{};
  if (owner) ::GetWindowRect(owner, &ownerRc);
  const int w = px(348);
  const int h = px(148);
  const int x = owner ? ownerRc.left + ((ownerRc.right - ownerRc.left) - w) / 2 : CW_USEDEFAULT;
  const int y = owner ? ownerRc.top + ((ownerRc.bottom - ownerRc.top) - h) / 2 : CW_USEDEFAULT;

  HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, owner,
                               nullptr, g_app.instance, nullptr);
  if (!dlg) {
    g_in = nullptr;
    return false;
  }

  if (owner) ::EnableWindow(owner, FALSE);
  ::SetFocus(st.edit);
  ::SendMessageW(st.edit, EM_SETSEL, 0, -1);

  MSG msg;
  while (::IsWindow(dlg) && ::GetMessageW(&msg, nullptr, 0, 0)) {
    if (!::IsDialogMessageW(dlg, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }

  if (owner) {
    ::EnableWindow(owner, TRUE);
    ::SetActiveWindow(owner);
  }
  g_in = nullptr;
  return st.accepted;
}

}  // namespace ui
