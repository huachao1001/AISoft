// `tray.run()` —— 创建隐藏窗口、挂上托盘图标、跑消息循环。
//
// ── 为什么必须有窗口 ──
//
// `Shell_NotifyIcon` 的 `hWnd` 是回调目标：用户点图标时系统往那个窗口发
// 消息。没有窗口就没有回调，所以托盘程序必须有一个（可以是不可见的）。
//
// ── 回调怎么拿到 Program ──
//
// 窗口过程是 C 函数，拿不到闭包。Program 通过一个文件级静态指针传进去，
// 只在 `run()` 的消息循环期间非空——消息循环结束时立刻清掉，避免下一次
// 脚本（热加载）用旧的程序地址。
//
// ── 为什么再次进入 VM 是安全的 ──
//
// 回调发生时外层 `execute` 还挂在栈上（`tray.run()` 就是它调的），但
// `Machine::callValue` 会新建自己的帧栈，不碰外层栈。这是 threading 用
// 「独立帧栈」的同一个设计。
#include "aipy/os_tray/tray.h"

#include <string>

#if defined(_WIN32)

#include <windows.h>
#include <shellapi.h>

#endif

namespace aipy {
namespace os_tray {
namespace {

#if defined(_WIN32)

// 托盘事件消息号。`Shell_NotifyIcon` 把「用户点了图标」发到注册时给的
// 消息号上，具体动作在 lParam 里。
constexpr UINT kTrayMessage = WM_APP + 1;

// 菜单命令 id 的起点。0 表示「用户点了别处没选任何项」。
constexpr UINT kMenuIdBase = 1;

constexpr const wchar_t* kWindowClass = L"AiPyTrayWindow";

// 正在跑消息循环的那个程序。窗口过程只能通过它拿到 Program。
const vm::Program* g_runningProgram = nullptr;

// 图标句柄：`g_iconOwned` 决定收尾时要不要 DestroyIcon。系统默认图标是
// 共享资源，销毁它属于未定义行为。
HICON g_icon = nullptr;
bool g_iconOwned = false;
bool g_installed = false;
HWND g_window = nullptr;

// 防止 `TrackPopupMenu` 的内层消息循环重新进入托盘事件处理。
//
// TrackPopupMenu 自己会泵消息，那些消息又会走到 windowProc；不加这个
// 守卫时右键菜单弹出期间再收到一次托盘事件就会递归建菜单。
bool g_inTrayEvent = false;

// UTF-8 → UTF-16。脚本里的路径与文本都是 UTF-8，而 Win32 的 W 系列要
// UTF-16。`std::wstring(utf8.begin(), utf8.end())` 是错的：它按字节拆，
// 一个汉字会被拆成几个无意义字符（中文提示显示成乱码）。这个坑踩过。
std::wstring toWide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()),
                                         nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                      &wide[0], needed);
  return wide;
}

// 加载图标：先按文件路径，失败退回系统默认。
//
// `LR_LOADFROMFILE` 才能读磁盘上的 .ico；系统默认图标走 LoadIcon，那是
// 共享句柄，`g_iconOwned` 因此为 false。
HICON loadIcon(const std::string& path) {
  if (!path.empty()) {
    const std::wstring wide = toWide(path);
    if (!wide.empty()) {
      HICON fromFile = static_cast<HICON>(LoadImageW(
          nullptr, wide.c_str(), IMAGE_ICON, 0, 0,
          LR_LOADFROMFILE | LR_DEFAULTSIZE));
      if (fromFile != nullptr) {
        g_iconOwned = true;
        return fromFile;
      }
      // 路径给了却读不到：报出来，别静默换默认图标——用户会以为路径对了。
      diagnose("tray.icon: cannot load icon '" + path + "'");
    }
  }
  g_iconOwned = false;
  // IDI_APPLICATION 自身就是 MAKEINTRESOURCEW(...)，再套一层会把它当整数
  // 截断成 WORD；这里只需显式转成 LPCWSTR 过 `/permissive-` 检查。
  return static_cast<HICON>(
      LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION)));
}

// 跑一个回调，失败时把原因写到诊断通道。
//
// 回调里抛出的异常不会向外传播：消息循环不是脚本的调用栈，抛出去也没有
// except 能接。报出来是唯一能做的事。
void runCallback(const vm::Program& program, const Value& callback,
                 const std::string& what) {
  const vm::Result result = vm::Machine().callValue(program, callback, {});
  if (!result.failed) return;
  diagnose("tray: callback '" + what + "' failed: " + result.error);
}

// 右键菜单。必须在显示前建好：TrackPopupMenu 返回时用户已经选完了。
void showMenu(HWND window) {
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) return;

  const State& state = os_tray::state();
  for (std::size_t i = 0; i < state.items.size(); ++i) {
    if (state.items[i].id == "-") {
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      continue;
    }
    const std::wstring label = toWide(state.items[i].label);
    AppendMenuW(menu, MF_STRING, kMenuIdBase + static_cast<UINT>(i),
                label.c_str());
  }
  if (state.items.empty()) {
    // 空菜单弹出来是一片空白框，看起来像 bug。给一条禁用的提示。
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuIdBase, L"(no menu items)");
  }

  // TrackPopupMenu 要求调用线程是前台窗口，否则菜单一弹出就消失（点一下
  // 别处就没了）。SetForegroundWindow 是官方文档给的做法。
  SetForegroundWindow(window);
  POINT cursor;
  GetCursorPos(&cursor);
  const UINT command = static_cast<UINT>(TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, window,
      nullptr));
  DestroyMenu(menu);
  // 弹完必须发一条空消息：否则菜单要等下次鼠标移动才彻底消失（Windows 的
  // 已知怪癖，官方示例也这么做）。
  PostMessageW(window, WM_NULL, 0, 0);

  if (command < kMenuIdBase) return;  // 用户点了别处
  const std::size_t index = command - kMenuIdBase;
  if (index >= state.items.size()) return;
  if (g_runningProgram == nullptr) return;

  Value callback;
  if (!findHandler(state.items[index].id, &callback)) return;
  runCallback(*g_runningProgram, callback, state.items[index].id);
}

// 处理一次托盘事件：右键弹菜单、左键跑 click 回调。
void handleTrayEvent(HWND window, LPARAM event) {
  if (g_inTrayEvent) return;
  g_inTrayEvent = true;

  // 没调 NIM_SETVERSION 时 lParam 就是鼠标消息本身。
  const UINT action = static_cast<UINT>(event);
  if (action == WM_RBUTTONUP || action == WM_CONTEXTMENU) {
    showMenu(window);
  } else if (action == WM_LBUTTONUP || action == WM_LBUTTONDBLCLK) {
    if (!os_tray::state().hasClickHandler || g_runningProgram == nullptr) return;
    Value callback;
    if (findHandler(kClickId, &callback)) {
      runCallback(*g_runningProgram, callback, kClickId);
    }
  }

  g_inTrayEvent = false;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
    case kTrayMessage:
      handleTrayEvent(window, lparam);
      return 0;

    case WM_CLOSE:
      DestroyWindow(window);
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

// 注册窗口类。第二次跑 `tray.run()` 时类已存在，那是可以接受的成功。
bool registerClass(HINSTANCE instance) {
  WNDCLASSEXW klass = {};
  klass.cbSize = sizeof(klass);
  // `CS_DBLCLKS`：要区分单击与双击就得有它，否则系统不产生 DBLCLK 消息。
  klass.style = CS_DBLCLKS;
  klass.lpfnWndProc = &windowProc;
  klass.hInstance = instance;
  klass.lpszClassName = kWindowClass;
  if (RegisterClassExW(&klass) != 0) return true;
  return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// 撤销图标 + 销毁窗口。顺序有讲究：先摘图标再销毁窗口，反过来时图标还
// 挂在已销毁的窗口上，系统会把那块托盘区域留成残留像素。
void teardown(NOTIFYICONDATAW* data) {
  if (g_installed && data != nullptr) {
    Shell_NotifyIconW(NIM_DELETE, data);
    g_installed = false;
  }
  if (g_iconOwned && g_icon != nullptr) DestroyIcon(g_icon);
  g_icon = nullptr;
  g_iconOwned = false;
  if (g_window != nullptr) {
    DestroyWindow(g_window);
    g_window = nullptr;
  }
}

#endif  // _WIN32

}  // namespace

bool run() {
#if !defined(_WIN32)
  diagnose("tray is only supported on Windows");
  return false;
#else
  const vm::Program* program = vm::currentProgram();
  if (program == nullptr) {
    diagnose("tray.run must be called from a script");
    return false;
  }

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  if (!registerClass(instance)) {
    diagnose("tray.run: cannot register the window class");
    return false;
  }

  // 隐藏窗口：托盘程序没有可见窗体，但必须有 HWND 才能收到图标点击。
  // 样式 0 而不是 `WS_VISIBLE`，所以系统不会画它。
  g_window = CreateWindowExW(0, kWindowClass, L"AiPy", 0, 0, 0, 0, 0, nullptr,
                             nullptr, instance, nullptr);
  if (g_window == nullptr) {
    diagnose("tray.run: cannot create the hidden window");
    return false;
  }

  const State& state = os_tray::state();

  // ── 挂图标 ──
  //
  // `NIF_MESSAGE` 让窗口收到点击消息；`NIF_ICON` / `NIF_TIP` 是图标与提示。
  // 不设 `NIF_INFO`：气泡提示走单独的 `tray.notify()`，混进注册项会让每次
  // 刷新图标都弹一次气泡。
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = g_window;
  data.uID = 1;  // 同一个窗口可挂多个图标，脚本模型里只用一个
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  data.uCallbackMessage = kTrayMessage;
  g_icon = loadIcon(state.iconPath);
  data.hIcon = g_icon;
  const std::wstring tooltip = toWide(state.tooltip);
  // szTip 是定长数组，超长由系统截断；手动截断留最后一个字符给 '\0'。
  const std::size_t limit = sizeof(data.szTip) / sizeof(data.szTip[0]) - 1;
  const std::size_t copied = tooltip.size() < limit ? tooltip.size() : limit;
  for (std::size_t i = 0; i < copied; ++i) data.szTip[i] = tooltip[i];
  data.szTip[copied] = L'\0';

  if (Shell_NotifyIconW(NIM_ADD, &data) == 0) {
    diagnose("tray.run: cannot add the tray icon");
    teardown(nullptr);
    return false;
  }
  g_installed = true;

  g_runningProgram = program;
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  g_runningProgram = nullptr;

  teardown(&data);
  return true;
#endif
}

void quit() {
#if defined(_WIN32)
  // 走 WM_CLOSE 而不是直接 PostQuitMessage：窗口销毁路径还要摘图标，
  // 跳过它会让图标留在托盘里直到用户把鼠标划过去。
  if (g_window != nullptr) PostMessageW(g_window, WM_CLOSE, 0, 0);
#endif
}

bool notify(const std::string& title, const std::string& text) {
#if !defined(_WIN32)
  (void)title;
  (void)text;
  return false;
#else
  // 气泡挂在图标上，所以只在图标已挂上时有效。没有图标却硬发，系统会
  // 静默忽略——那比返回 False 更难查。
  if (!g_installed || g_window == nullptr) return false;

  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = g_window;
  data.uID = 1;
  data.uFlags = NIF_INFO;
  data.dwInfoFlags = NIIF_INFO;

  const std::wstring wideTitle = toWide(title);
  const std::size_t titleLimit =
      sizeof(data.szInfoTitle) / sizeof(data.szInfoTitle[0]) - 1;
  const std::size_t titleCopied =
      wideTitle.size() < titleLimit ? wideTitle.size() : titleLimit;
  for (std::size_t i = 0; i < titleCopied; ++i) {
    data.szInfoTitle[i] = wideTitle[i];
  }
  data.szInfoTitle[titleCopied] = L'\0';

  const std::wstring wideText = toWide(text);
  const std::size_t textLimit =
      sizeof(data.szInfo) / sizeof(data.szInfo[0]) - 1;
  const std::size_t textCopied =
      wideText.size() < textLimit ? wideText.size() : textLimit;
  for (std::size_t i = 0; i < textCopied; ++i) data.szInfo[i] = wideText[i];
  data.szInfo[textCopied] = L'\0';

  return Shell_NotifyIconW(NIM_MODIFY, &data) != 0;
#endif
}

}  // namespace os_tray
}  // namespace aipy
