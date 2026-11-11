// 主窗口的原生宿主接入：拖放与单实例转发
//
// 界面与生命周期在 MainActivity 里，这里只处理绕不开的平台消息。
// 注意 AiUI 的顶层窗口类名是 "AiUIWindow"，不是本工程另起的名字：
// 把 DragAcceptFiles 与窗口子类挂到主窗口句柄上才收得到消息。
#include "ui_main.h"

#include "app.h"
#include "dlg_compress.h"
#include "sevenzip_loader.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>

#include <vector>

namespace ui {
namespace {

// 子窗口（光标实际所在）收不到拖放时转给宿主窗口
HWND DropOwner(HWND child) {
  HWND parent = ::GetParent(child);
  return parent ? parent : child;
}

// 单实例转发：另一个进程把路径塞进 WM_COPYDATA
void HandleCopyData(const COPYDATASTRUCT* cds) {
  if (!cds || cds->dwData != 1 || !cds->lpData || cds->cbData == 0) return;
  const std::wstring arg(static_cast<const wchar_t*>(cds->lpData));
  if (util::IsDirectory(arg)) {
    AppNavigateTo(arg);
  } else if (arc::Archive::IsArchiveFile(arg) && util::PathExists(arg)) {
    AppOpenArchive(arg);
  }
}

LRESULT CALLBACK HostHookProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                              DWORD_PTR) {
  switch (msg) {
    case WM_DROPFILES: {
      // 顶层窗口自己处理；子窗口转给宿主，避免"发给自己"的死循环
      HWND owner = DropOwner(hwnd);
      if (owner != hwnd) {
        ::SendMessageW(owner, WM_DROPFILES, wParam, lParam);
      } else {
        HandleDroppedFiles(hwnd, reinterpret_cast<HDROP>(wParam));
      }
      return 0;
    }
    case WM_COPYDATA:
      HandleCopyData(reinterpret_cast<const COPYDATASTRUCT*>(lParam));
      return TRUE;
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, HostHookProc, id);
      break;
    default:
      break;
  }
  return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

// 与 AiUI 平台层的顶层窗口类名一致；单实例转发靠它找窗口
constexpr const wchar_t* kHostClass = L"AiUIWindow";

}  // namespace

void HandleDroppedFiles(HWND host, HDROP drop) {
  const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  std::vector<std::wstring> paths;
  for (UINT i = 0; i < count; ++i) {
    wchar_t buffer[MAX_PATH * 2];
    ::DragQueryFileW(drop, i, buffer, MAX_PATH * 2);
    paths.push_back(buffer);
  }
  ::DragFinish(drop);
  if (paths.empty()) return;
  if (paths.size() == 1 && arc::Archive::IsArchiveFile(paths[0])) {
    AppOpenArchive(paths[0]);
  } else {
    ShowCompressDialog(host, paths);
  }
}

void AttachHostHooks(HWND host) {
  if (!host) return;
  ::DragAcceptFiles(host, TRUE);
  ::SetWindowSubclass(host, HostHookProc, 1, 0);
}

bool ForwardPathToRunningWindow(const std::wstring& path) {
  HWND existing = ::FindWindowW(kHostClass, nullptr);
  if (!existing) return false;
  if (!path.empty()) {
    COPYDATASTRUCT cds{};
    cds.dwData = 1;
    cds.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(path.c_str());
    ::SendMessageW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
  }
  if (::IsIconic(existing)) ::ShowWindow(existing, SW_RESTORE);
  ::SetForegroundWindow(existing);
  return true;
}

}  // namespace ui
