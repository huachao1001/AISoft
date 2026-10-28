// 轻量输入框
#include "dlg_input.h"

#include "app.h"
#include "ui.h"

#include <commctrl.h>

namespace ui {
namespace {

constexpr const wchar_t* kClass = L"AIZipInputDlg";
constexpr int kIdEdit = 4001;
constexpr int kIdOk = IDOK;
constexpr int kIdCancel = IDCANCEL;

struct InputState {
  std::wstring label;
  std::wstring* value = nullptr;
  bool password = false;
  bool accepted = false;
  HWND edit = nullptr;
};

InputState* g_in = nullptr;

void CreateControls(HWND hwnd) {
  CreateDlgItem(hwnd, L"STATIC", g_in->label.c_str(), SS_LEFT, 14, 14, 316, 18, 0);
  g_in->edit = CreateDlgItem(hwnd, L"EDIT", g_in->value->c_str(),
                             ES_AUTOHSCROLL | (g_in->password ? ES_PASSWORD : 0) | WS_TABSTOP, 14,
                             36, 316, 24, kIdEdit, WS_EX_CLIENTEDGE);
  CreateDlgItem(hwnd, L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP, 194, 74, 66, 26, kIdOk);
  CreateDlgItem(hwnd, L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 266, 74, 66, 26, kIdCancel);

  for (HWND c = ::GetWindow(hwnd, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT)) {
    ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui), TRUE);
  }
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
      CreateControls(hwnd);
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
    registered = RegisterWindowClass(kClass, Proc, g_app.instance, CS_HREDRAW | CS_VREDRAW,
                                     reinterpret_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
  }

  InputState st;
  st.label = label;
  st.value = &value;
  st.password = password;
  g_in = &st;

  const int w = g_app.px(348);
  const int h = g_app.px(148);
  const RECT dr = DialogRect(owner, w, h);

  HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, dr.left, dr.top, w,
                               h, owner, nullptr, g_app.instance, nullptr);
  if (!dlg) {
    g_in = nullptr;
    return false;
  }

  ::SetFocus(st.edit);
  ::SendMessageW(st.edit, EM_SETSEL, 0, -1);
  RunModalDialog(dlg, owner);
  g_in = nullptr;
  return st.accepted;
}

}  // namespace ui
