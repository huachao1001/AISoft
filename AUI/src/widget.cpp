#include "AUI/widget.h"

#include "AUI/platform.h"

#include <map>

namespace AUI {
namespace {

std::map<HWND, Widget*>& Instances() {
  static std::map<HWND, Widget*> map;
  return map;
}

}  // namespace

bool RegisterWidgetClass(const wchar_t* name, HINSTANCE inst, UINT style) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = style | CS_DBLCLKS;
  wc.lpfnWndProc = Widget::WndProc;
  wc.hInstance = inst;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = name;
  return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND Widget::Create(HWND parent, HINSTANCE inst, int id, const wchar_t* className, DWORD style,
                    DWORD exStyle) {
  RegisterWidgetClass(className, inst);
  // 用 this 作为 lpCreateParams，WM_NCCREATE 时就能绑定实例
  hwnd_ = ::CreateWindowExW(exStyle, className, L"", style, 0, 0, 0, 0, parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, this);
  if (hwnd_) dpi_ = DpiForWindow(hwnd_);
  return hwnd_;
}

LRESULT CALLBACK Widget::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_NCCREATE) {
    auto* self = static_cast<Widget*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
    if (self) {
      // 提前绑定句柄：OnCreated 里要拿它当父窗口建子控件
      self->hwnd_ = hwnd;
      Instances()[hwnd] = self;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  auto& map = Instances();
  auto it = map.find(hwnd);
  Widget* self = (it != map.end()) ? it->second : nullptr;

  if (msg == WM_NCDESTROY) {
    LRESULT r = self ? self->OnMessage(msg, wParam, lParam)
                     : ::DefWindowProcW(hwnd, msg, wParam, lParam);
    map.erase(hwnd);
    return r;
  }
  if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

  if (msg == WM_CREATE) {
    const LRESULT r = ::DefWindowProcW(hwnd, msg, wParam, lParam);
    self->OnCreated();
    return r;
  }

  switch (msg) {
    case WM_SIZE:
      self->dpi_ = DpiForWindow(hwnd);
      self->OnSize(LOWORD(lParam), HIWORD(lParam));
      return 0;
    case WM_ERASEBKGND:
      if (self->OnEraseBkgnd(reinterpret_cast<HDC>(wParam))) return 1;
      break;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd, &ps);
      self->OnPaint();
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      break;
  }
  return self->OnMessage(msg, wParam, lParam);
}

}  // namespace AUI
