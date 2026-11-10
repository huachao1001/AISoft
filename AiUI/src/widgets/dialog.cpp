// 宿主接入层实现：窗口类、居中、模态循环
#include "AiUI/widgets/dialog.h"

namespace AiUI {
namespace {

// 只有本进程的窗口才能当模态 owner，否则异常退出会锁死外部界面
bool isOwnWindow(HWND hwnd) {
  if (!hwnd || !::IsWindow(hwnd)) return false;
  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  return pid == ::GetCurrentProcessId();
}

}  // namespace

bool registerWindowClass(const wchar_t* name, WNDPROC proc, HINSTANCE instance, UINT style,
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

RECT dialogRect(HWND owner, int width, int height) {
  RECT out{CW_USEDEFAULT, CW_USEDEFAULT, width, height};
  RECT reference{};
  if ((owner && ::GetWindowRect(owner, &reference)) ||
      ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &reference, 0)) {
    out.left = reference.left + ((reference.right - reference.left) - width) / 2;
    out.top = reference.top + ((reference.bottom - reference.top) - height) / 2;
  }
  return out;
}

void runModalDialog(HWND dialog, HWND owner, const std::function<void()>& onBeforeEnable) {
  if (!isOwnWindow(owner)) owner = nullptr;
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

}  // namespace AiUI
