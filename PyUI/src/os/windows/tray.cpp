// Windows 托盘：Shell_NotifyIcon + 每次现取菜单
//
// 托盘图标需要一个「消息窗口」才能收到 NIN_SELECT / WM_RBUTTONUP，
// 这跟界面树的主窗口是两回事——主窗口可能被销毁，而托盘还在。
// 所以这里自己建一个隐藏的 message-only 窗口（HWND_MESSAGE）。
//
// 菜单每次右键现取（见 platform/tray.h 的理由）：托盘项的可用性依赖
// 运行状态，缓存一定会漏。
#include "win_platform_hosts.h"

#include "pyui/menu.h"
#include "pyui/platform/context_menu.h"

#include "win_common.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pyui {
namespace {

constexpr const wchar_t* kTrayClassName = L"PyUITrayWindow";
constexpr UINT kTrayIconId = 1;
// 自定义消息：Shell_NotifyIcon 用它在老系统上送右键/单击。
// NIN_SELECT 之后还送 WM_CONTEXTMENU，所以只认下面两个就够。
constexpr UINT kTrayCallbackMessage = WM_APP + 1;

  // 托盘图标的句柄：从本 exe 的主图标取。
  // 不额外引一套图片资源约定——应用已经有 exe 图标了，托盘就该用它。
  HICON loadTrayIcon() {
    HINSTANCE instance = win32::moduleHandle();
    // 用小的那档：托盘区是 16×16（高 DPI 下 20/24），取大图再缩会糊。
    HICON icon = static_cast<HICON>(
        ::LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                     ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), 0));
    if (icon != nullptr) return icon;
    return ::LoadIconW(nullptr, IDI_APPLICATION);
  }

}  // namespace

WindowsTray::WindowsTray() = default;

WindowsTray::~WindowsTray() {
  // 托盘图标必须先删：窗口没了之后 Shell_NotifyIcon(NIM_DELETE) 会找不到
  // 那个图标，注销时托盘区就留一个死图标直到鼠标划过。
  hide();
  if (hwnd_ != nullptr) {
    ::DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

bool WindowsTray::ensureWindow() {
  if (hwnd_ != nullptr) return true;

  if (win32::registerWindowClass(kTrayClassName, &WindowsTray::windowProc, nullptr, nullptr, 0) ==
      0) {
    return false;
  }

  // HWND_MESSAGE：消息窗口。不出现在任务栏、alt-tab、也不参与 Z 序。
  hwnd_ = ::CreateWindowExW(0, kTrayClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                            win32::moduleHandle(), this);
  return hwnd_ != nullptr;
}

LRESULT CALLBACK WindowsTray::windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  WindowsTray* self = nullptr;
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    self = static_cast<WindowsTray*>(create->lpCreateParams);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self != nullptr) self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<WindowsTray*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self == nullptr) return ::DefWindowProcW(hwnd, message, wparam, lparam);
  return self->processMessage(hwnd, message, wparam, lparam);
}

LRESULT WindowsTray::processMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case kTrayCallbackMessage: {
      // 低 16 位是鼠标消息。新版 shell 用 NIN_SELECT / NIN_KEYSELECT，
      // 老版用 WM_LBUTTONUP；两个都认。
      const UINT event = LOWORD(lparam);
      if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) {
        if (onActivate_) onActivate_();
        return 0;
      }
      if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
        showMenu();
        return 0;
      }
      return 0;
    }

    case WM_COMMAND:
      // 菜单用 TrackPopupMenuEx 的 TPM_RETURNCMD 形式，选中项直接从返回值
      // 拿到，不会再走 WM_COMMAND。保留这个分支是为了让「有意外消息」
      // 不落到 DefWindowProc（那会响一声提示音）。
      return 0;

    case WM_DESTROY:
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      hwnd_ = nullptr;
      return 0;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

void WindowsTray::fillNotifyData(NOTIFYICONDATAW& data) const {
  data = {};
  data.cbSize = sizeof(data);
  data.hWnd = hwnd_;
  data.uID = kTrayIconId;
  // NIF_MESSAGE：要让窗口收到点击。NIF_ICON：图标。NIF_TIP：悬停文字。
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  data.uCallbackMessage = kTrayCallbackMessage;
  data.hIcon = loadTrayIcon();
  // szTip 只有 128 个 wchar：超长会被系统静默截断，这里先截。
  const size_t count = (std::min)(tooltip_.size(), static_cast<size_t>(127));
  std::copy_n(tooltip_.c_str(), count, data.szTip);
  data.szTip[count] = L'\0';
}

bool WindowsTray::show(const std::string& tooltip) {
  if (!ensureWindow()) return false;
  tooltip_ = win32::toWide(tooltip);

  NOTIFYICONDATAW data{};
  fillNotifyData(data);

  if (visible_) {
    // 已经在托盘里：走 NIM_MODIFY。再 NIM_ADD 会失败（同一个 uID 重复添加）。
    if (::Shell_NotifyIconW(NIM_MODIFY, &data) == 0) return false;
  } else {
    if (::Shell_NotifyIconW(NIM_ADD, &data) == 0) return false;
    visible_ = true;
    // 新版 shell 支持「版本」握手：告知系统我们懂 NIM_SETVERSION 的语义，
    // 之后才会收到 NIN_SELECT 而不是只在鼠标移动时收到 WM_MOUSEMOVE。
    data.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &data);
  }
  return true;
}

void WindowsTray::hide() {
  if (!visible_ || hwnd_ == nullptr) {
    visible_ = false;
    return;
  }
  NOTIFYICONDATAW data{};
  fillNotifyData(data);
  ::Shell_NotifyIconW(NIM_DELETE, &data);
  visible_ = false;
}

void WindowsTray::setTooltip(const std::string& tooltip) {
  tooltip_ = win32::toWide(tooltip);
  if (!visible_ || hwnd_ == nullptr) return;
  NOTIFYICONDATAW data{};
  fillNotifyData(data);
  data.uFlags = NIF_TIP;
  ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void WindowsTray::showMenu() {
  if (!menuProvider_ || hwnd_ == nullptr) return;
  const std::vector<MenuItem> items = menuProvider_();
  if (items.empty()) return;

  // 托盘菜单必须用「当前鼠标位置」而不是窗口坐标：托盘图标不在我们的
  // 窗口里，它是任务栏上的一个区域。
  POINT cursor{};
  ::GetCursorPos(&cursor);

  PopupMenuRequest request;
  request.owner = hwnd_;
  request.screenPosition = Point{cursor.x, cursor.y};
  request.items = items;

  // 走同一个 ContextMenuHost：托盘菜单和右键菜单在系统层面应当完全一致
  // （同样的字体、同样的键盘导航、同样的边缘避让）。
  static WindowsContextMenu menu;
  const std::string selected = menu.showPopupMenu(request);
  if (!selected.empty() && menuHandler_) menuHandler_(selected);
}

}  // namespace pyui
