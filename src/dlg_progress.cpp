// 进度对话框：工作线程跑压缩/解压，UI 线程只负责刷新
#include "dlg_progress.h"

#include "app.h"
#include "ui.h"
#include "util.h"

#include <commctrl.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace ui {
namespace {

constexpr const wchar_t* kClass = L"MyZipProgressDlg";
constexpr UINT kMsgProgress = WM_APP + 1;
constexpr UINT kMsgSyncCall = WM_APP + 3;
constexpr UINT kMsgDone = WM_APP + 4;
constexpr int kIdBar = 3001;
constexpr int kIdCancel = 3002;

struct ProgressState {
  WorkFn work;
  HWND owner = nullptr;
  HWND wnd = nullptr;
  HWND bar = nullptr;
  HWND labelFile = nullptr;
  HWND labelPct = nullptr;
  HWND cancelBtn = nullptr;

  std::atomic<bool> cancel{false};
  std::atomic<int> lastPostTick{0};

  std::mutex mu;
  std::wstring currentFile;
  uint64_t completed = 0;
  uint64_t total = 0;

  // 工作线程投递给 UI 线程的同步调用
  std::mutex callMu;
  std::deque<std::function<void()>> calls;

  bool ok = false;
  std::thread worker;
};

ProgressState* g_state = nullptr;

// 进度快照投递给 UI 线程；限流 60ms，避免消息洪水
bool ReportProgress(uint64_t completed, uint64_t total, const std::wstring& current) {
  ProgressState* st = g_state;
  if (!st) return true;
  if (st->cancel.load(std::memory_order_relaxed)) return false;

  {
    std::lock_guard<std::mutex> lock(st->mu);
    st->completed = completed;
    st->total = total;
    if (!current.empty()) st->currentFile = current;
  }

  const int now = ::GetTickCount();
  const int last = st->lastPostTick.load(std::memory_order_relaxed);
  if (now - last >= 60) {
    st->lastPostTick.store(now, std::memory_order_relaxed);
    if (st->wnd) ::PostMessageW(st->wnd, kMsgProgress, 0, 0);
  }
  return !st->cancel.load(std::memory_order_relaxed);
}

void CreateControls(HWND hwnd, HINSTANCE inst) {
  const int dpi = g_app.dpi;
  auto px = [&](int v) { return ::MulDiv(v, dpi, 96); };
  auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
                  int id) {
    return ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w),
                             px(h), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst,
                             nullptr);
  };

  make(L"STATIC", L"正在处理，请稍候…", SS_LEFT, 14, 14, 340, 18, 0);
  g_state->labelFile = make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 14, 36, 340, 18, 0);
  g_state->bar = make(PROGRESS_CLASSW, L"", PBS_SMOOTH, 14, 60, 340, 18, kIdBar);
  g_state->labelPct = make(L"STATIC", L"0%", SS_LEFT, 14, 82, 340, 16, 0);
  g_state->cancelBtn = make(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 286, 106, 68, 26,
                            kIdCancel);

  ::SendMessageW(g_state->bar, PBM_SETRANGE32, 0, 1000);
  ::SendMessageW(g_state->bar, PBM_SETPOS, 0, 0);

  for (HWND c = ::GetWindow(hwnd, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT)) {
    ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui), TRUE);
  }
}

void ApplyProgress() {
  ProgressState* st = g_state;
  if (!st || !st->wnd) return;

  std::wstring file;
  uint64_t completed = 0;
  uint64_t total = 0;
  {
    std::lock_guard<std::mutex> lock(st->mu);
    file = st->currentFile;
    completed = st->completed;
    total = st->total;
  }

  int pos = 0;
  if (total > 0) {
    pos = static_cast<int>(completed * 1000 / total);
    if (pos > 1000) pos = 1000;
  }
  ::SendMessageW(st->bar, PBM_SETPOS, pos, 0);

  std::wstring pct = std::to_wstring(total > 0 ? completed * 100 / total : 0) + L"%";
  if (total > 0) {
    pct += L"   " + util::FormatSize(completed) + L" / " + util::FormatSize(total);
  } else if (completed > 0) {
    pct += L"   " + util::FormatSize(completed);
  }
  ::SetWindowTextW(st->labelPct, pct.c_str());

  if (!file.empty()) {
    ::SetWindowTextW(st->labelFile, (L"当前文件：" + util::BaseName(file)).c_str());
  }
}

void DrainSyncCalls() {
  ProgressState* st = g_state;
  if (!st) return;
  for (;;) {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lock(st->callMu);
      if (st->calls.empty()) break;
      fn = std::move(st->calls.front());
      st->calls.pop_front();
    }
    if (fn) fn();
  }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      g_state->wnd = hwnd;
      CreateControls(hwnd, reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)));
      return 0;

    case kMsgProgress:
      ApplyProgress();
      return 0;

    case kMsgSyncCall:
      DrainSyncCalls();
      ::SetEvent(reinterpret_cast<HANDLE>(lParam));
      return 0;

    case kMsgDone:
      ApplyProgress();
      ::DestroyWindow(hwnd);
      return 0;

    case WM_COMMAND:
      if (LOWORD(wParam) == kIdCancel && g_state) {
        g_state->cancel.store(true, std::memory_order_relaxed);
        ::EnableWindow(g_state->cancelBtn, FALSE);
        ::SetWindowTextW(g_state->cancelBtn, L"正在取消…");
        return 0;
      }
      break;

    case WM_CLOSE:
      if (g_state) g_state->cancel.store(true, std::memory_order_relaxed);
      return 0;

    case WM_DESTROY:
      return 0;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool ShowProgressDialog(HWND owner, const std::wstring& title, const WorkFn& work) {
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

  ProgressState state;
  state.work = work;
  state.owner = owner;
  g_state = &state;

  const int dpi = g_app.dpi;
  auto px = [&](int v) { return ::MulDiv(v, dpi, 96); };

  RECT ownerRc{};
  if (owner) ::GetWindowRect(owner, &ownerRc);
  const int w = px(380);
  const int h = px(150);
  const int x = owner ? ownerRc.left + ((ownerRc.right - ownerRc.left) - w) / 2 : CW_USEDEFAULT;
  const int y = owner ? ownerRc.top + ((ownerRc.bottom - ownerRc.top) - h) / 2 : CW_USEDEFAULT;

  HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, owner,
                               nullptr, g_app.instance, nullptr);
  if (!dlg) {
    g_state = nullptr;
    return false;
  }

  if (owner) ::EnableWindow(owner, FALSE);

  state.worker = std::thread([&state]() {
    bool ok = false;
    try {
      ok = state.work([](uint64_t c, uint64_t t, const std::wstring& cur) {
        return ReportProgress(c, t, cur);
      });
    } catch (...) {
      ok = false;
    }
    state.ok = ok;
    if (state.wnd) ::PostMessageW(state.wnd, kMsgDone, 0, 0);
  });

  MSG msg;
  while (::IsWindow(dlg) && ::GetMessageW(&msg, nullptr, 0, 0)) {
    if (!::IsDialogMessageW(dlg, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }

  if (state.worker.joinable()) state.worker.join();
  if (owner) {
    ::EnableWindow(owner, TRUE);
    ::SetActiveWindow(owner);
  }
  g_state = nullptr;
  return state.ok;
}

void RunOnUiThread(const std::function<void()>& fn) {
  ProgressState* st = g_state;
  if (!st || !st->wnd) {
    fn();
    return;
  }
  if (::GetCurrentThreadId() == ::GetWindowThreadProcessId(st->wnd, nullptr)) {
    fn();
    return;
  }
  HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  {
    std::lock_guard<std::mutex> lock(st->callMu);
    st->calls.push_back(fn);
  }
  ::PostMessageW(st->wnd, kMsgSyncCall, 0, reinterpret_cast<LPARAM>(done));
  ::WaitForSingleObject(done, INFINITE);
  ::CloseHandle(done);
}

}  // namespace ui
