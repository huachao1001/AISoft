// 解压对话框：对齐 HaoZip 的“解压”窗口
#include "dlg_extract.h"

#include "app.h"
#include "archive.h"
#include "dlg_input.h"
#include "dlg_progress.h"
#include "ui.h"
#include "ui_list.h"
#include "util.h"

#include <commctrl.h>
#include <dlgs.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <string>
#include <vector>

namespace ui {
namespace {

constexpr const wchar_t* kClass = L"MyZipExtractDlg";

// 对话框非横幅区域底色
constexpr COLORREF kDlgBg = RGB(240, 247, 252);

constexpr int kIdTarget = 2101;
constexpr int kIdBrowse = 2102;
constexpr int kIdNewFolder = 2103;
constexpr int kIdUpdate1 = 2110;   // 解压并替换
constexpr int kIdUpdate2 = 2111;   // 解压并更新
constexpr int kIdUpdate3 = 2112;   // 仅更新已存在
constexpr int kIdOver1 = 2120;     // 询问
constexpr int kIdOver2 = 2121;     // 覆盖
constexpr int kIdOver3 = 2122;     // 跳过
constexpr int kIdOver4 = 2123;     // 重命名
constexpr int kIdOptPath = 2130;   // 在目标保留完整路径
constexpr int kIdOptOpen = 2131;   // 完成后打开文件夹
constexpr int kIdOptDelete = 2132; // 完成后删除压缩包
constexpr int kIdPassword = 2133;
constexpr int kIdSaveDefault = 2134;
constexpr int kIdGroupUpdate = 2135;
constexpr int kIdGroupOver = 2136;
constexpr int kIdGroupOther = 2137;
constexpr int kIdOk = IDOK;
constexpr int kIdCancel = IDCANCEL;

struct DlgState {
  std::vector<int> selected;
  std::wstring targetDir;
  int updateIndex = 0;
  int overwriteIndex = 1;  // 默认覆盖
  bool openAfter = true;
  bool deleteAfter = false;
  std::wstring password;
};

DlgState* g_state = nullptr;

std::wstring DefaultTargetDir() {
  if (g_app.archiveMode) {
    const std::wstring dir = util::DirName(g_app.archivePath);
    const std::wstring stem = util::FileStem(g_app.archivePath);
    return util::JoinPath(dir, stem);
  }
  if (!g_app.settings.extractDir.empty()) return g_app.settings.extractDir;
  return g_app.curDir;
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

  make(L"STATIC", L"目标路径(T)", SS_LEFT, 12, 12, 160, 16, 0);
  make(L"EDIT", g_state->targetDir.c_str(), WS_BORDER | ES_AUTOHSCROLL, 12, 30, 380, 22, kIdTarget);
  make(L"BUTTON", L"浏览(B)...", BS_PUSHBUTTON | WS_TABSTOP, 398, 29, 84, 24, kIdBrowse);
  make(L"BUTTON", L"新建文件夹(N)", BS_PUSHBUTTON | WS_TABSTOP, 398, 57, 84, 24, kIdNewFolder);

  // 更新方式
  make(L"BUTTON", L"更新方式", BS_GROUPBOX, 12, 72, 380, 74, kIdGroupUpdate);
  make(L"BUTTON", L"解压并替换文件(R)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 22, 92, 350,
       18, kIdUpdate1);
  make(L"BUTTON", L"解压并更新文件(U)", BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 112, 350, 18,
       kIdUpdate2);
  make(L"BUTTON", L"仅更新已存在的文件(E)", BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 132, 350, 18,
       kIdUpdate3);
  ::SendMessageW(::GetDlgItem(hwnd, kIdUpdate1), BM_SETCHECK, BST_CHECKED, 0);

  // 覆盖方式
  make(L"BUTTON", L"覆盖方式", BS_GROUPBOX, 12, 152, 380, 100, kIdGroupOver);
  make(L"BUTTON", L"覆盖前询问(A)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 22, 172, 350, 18,
       kIdOver1);
  make(L"BUTTON", L"直接覆盖，不询问(W)", BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 192, 350, 18,
       kIdOver2);
  make(L"BUTTON", L"跳过已存在的文件(S)", BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 212, 350, 18,
       kIdOver3);
  make(L"BUTTON", L"自动重命名已存在的文件(R)", BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 232, 350, 18,
       kIdOver4);
  ::SendMessageW(::GetDlgItem(hwnd, kIdOver2), BM_SETCHECK, BST_CHECKED, 0);

  // 其他选项
  make(L"BUTTON", L"其他选项", BS_GROUPBOX, 12, 258, 380, 84, kIdGroupOther);
  HWND openChk = make(L"BUTTON", L"解压完成后打开目标文件夹(O)", BS_AUTOCHECKBOX | WS_TABSTOP, 22,
                      276, 350, 18, kIdOptOpen);
  ::SendMessageW(openChk, BM_SETCHECK, BST_CHECKED, 0);
  make(L"BUTTON", L"解压完成后删除压缩包(D)", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 296, 350, 18,
       kIdOptDelete);
  make(L"BUTTON", L"密码(P)...", BS_PUSHBUTTON | WS_TABSTOP, 22, 314, 84, 22, kIdPassword);

  make(L"BUTTON", L"保存为默认(S)", BS_PUSHBUTTON | WS_TABSTOP, 12, 354, 110, 24, kIdSaveDefault);
  make(L"BUTTON", L"确定(O)", BS_DEFPUSHBUTTON | WS_TABSTOP, 320, 354, 68, 26, kIdOk);
  make(L"BUTTON", L"取消(C)", BS_PUSHBUTTON | WS_TABSTOP, 394, 354, 68, 26, kIdCancel);

  for (HWND c = ::GetWindow(hwnd, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT)) {
    ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui), TRUE);
  }
}

void PaintBanner(HWND hwnd) {
  // 横幅已取消，只负责铺一遍对话框底色；
  // 否则吞掉 WM_ERASEBKGND 后会露出未初始化的黑底
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  HDC target = ::GetDC(hwnd);
  ui::MemDC mem(target, rc);
  HDC dc = mem.dc();
  FillRect(dc, rc, kDlgBg);
  mem.Commit();
  ::ReleaseDC(hwnd, target);
}

void OnBrowse(HWND hwnd) {
  BROWSEINFOW bi{};
  bi.hwndOwner = hwnd;
  bi.lpszTitle = L"选择解压目标文件夹";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  LPITEMIDLIST pidl = ::SHBrowseForFolderW(&bi);
  if (!pidl) return;
  wchar_t path[MAX_PATH];
  if (::SHGetPathFromIDListW(pidl, path)) {
    ::SetWindowTextW(::GetDlgItem(hwnd, kIdTarget), path);
  }
  ::CoTaskMemFree(pidl);
}

void OnNewFolder(HWND hwnd) {
  wchar_t cur[MAX_PATH * 2];
  ::GetWindowTextW(::GetDlgItem(hwnd, kIdTarget), cur, MAX_PATH * 2);
  std::wstring name;
  if (!PromptText(hwnd, L"新建文件夹", L"文件夹名称：", name)) return;
  if (name.empty()) return;
  std::wstring path = util::JoinPath(cur, name);
  if (!util::EnsureDirectory(path)) {
    ::MessageBoxW(hwnd, L"无法创建文件夹。", L"MyZip", MB_OK | MB_ICONERROR);
    return;
  }
  ::SetWindowTextW(::GetDlgItem(hwnd, kIdTarget), path.c_str());
}

void OnPassword(HWND hwnd) {
  std::wstring pwd = g_state->password;
  if (PromptText(hwnd, L"输入密码", L"密码：", pwd, true)) {
    g_state->password = pwd;
  }
}

void OnOk(HWND hwnd) {
  wchar_t buf[MAX_PATH * 2];
  ::GetWindowTextW(::GetDlgItem(hwnd, kIdTarget), buf, MAX_PATH * 2);
  std::wstring target = buf;
  if (target.empty()) {
    ::MessageBoxW(hwnd, L"请填写解压目标路径。", L"MyZip", MB_OK | MB_ICONINFORMATION);
    return;
  }
  target = util::AbsolutePath(target);
  if (!util::EnsureDirectory(target)) {
    ::MessageBoxW(hwnd, L"无法创建目标文件夹。", L"MyZip", MB_OK | MB_ICONERROR);
    return;
  }

  const bool openAfter = ::SendMessageW(::GetDlgItem(hwnd, kIdOptOpen), BM_GETCHECK, 0, 0) ==
                         BST_CHECKED;
  const bool deleteAfter = ::SendMessageW(::GetDlgItem(hwnd, kIdOptDelete), BM_GETCHECK, 0, 0) ==
                           BST_CHECKED;

  arc::ExtractOptions opts;
  opts.targetDir = target;
  opts.password = g_state->password;

  if (::SendMessageW(::GetDlgItem(hwnd, kIdUpdate3), BM_GETCHECK, 0, 0) == BST_CHECKED) {
    opts.update = arc::ExtractOptions::Update::kNewerOnly;
  } else {
    opts.update = arc::ExtractOptions::Update::kExtractReplace;
  }

  if (::SendMessageW(::GetDlgItem(hwnd, kIdOver1), BM_GETCHECK, 0, 0) == BST_CHECKED) {
    opts.overwrite = arc::ExtractOptions::Overwrite::kAsk;
  } else if (::SendMessageW(::GetDlgItem(hwnd, kIdOver3), BM_GETCHECK, 0, 0) == BST_CHECKED) {
    opts.overwrite = arc::ExtractOptions::Overwrite::kSkip;
  } else if (::SendMessageW(::GetDlgItem(hwnd, kIdOver4), BM_GETCHECK, 0, 0) == BST_CHECKED) {
    opts.overwrite = arc::ExtractOptions::Overwrite::kRename;
  } else {
    opts.overwrite = arc::ExtractOptions::Overwrite::kOverwrite;
  }

  // 收集要解压的成员下标
  std::vector<uint32_t> indices;
  for (int row : g_state->selected) {
    if (row < 0 || row >= static_cast<int>(g_app.items.size())) continue;
    const Item& it = g_app.items[row];
    const std::wstring prefix =
        g_app.innerPath.empty() ? it.name : g_app.innerPath + L"\\" + it.name;
    if (it.isDir) {
      // 目录：带上其下全部成员
      for (size_t i = 0; i < g_app.archive.Entries().size(); ++i) {
        const std::wstring& p = g_app.archive.Entries()[i].path;
        if (::CompareStringOrdinal(p.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                                   static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL) {
          indices.push_back(static_cast<uint32_t>(i));
        }
      }
    } else if (it.entryIndex >= 0) {
      indices.push_back(static_cast<uint32_t>(it.entryIndex));
    }
  }

  g_app.settings.extractDir = target;
  g_app.settings.openAfterExtract = openAfter;

  ::DestroyWindow(hwnd);

  std::wstring err;
  const bool ok = ShowProgressDialog(
      g_app.mainWnd, L"正在解压", [&](const arc::ProgressFn& prog) {
        return g_app.archive.Extract(
            indices, opts, prog,
            [](const std::wstring& path) -> int {
              int result = 1;
              RunOnUiThread([&]() {
                const std::wstring msg = L"文件已存在：\n\n" + path + L"\n\n是否覆盖？";
                const int r = ::MessageBoxW(g_app.mainWnd, msg.c_str(), L"MyZip",
                                            MB_YESNO | MB_ICONWARNING);
                result = (r == IDYES) ? 1 : 0;
              });
              return result;
            },
            [&](std::wstring& out) -> bool {
              bool accepted = false;
              RunOnUiThread([&]() {
                std::wstring pwd;
                accepted = PromptText(g_app.mainWnd, L"输入密码",
                                      L"压缩包已加密，请输入密码：", pwd, true);
                if (accepted) out = pwd;
              });
              return accepted;
            },
            err);
      });

  if (!ok) {
    if (!err.empty()) {
      ::MessageBoxW(g_app.mainWnd, (L"解压失败：\n" + err).c_str(), L"MyZip",
                    MB_OK | MB_ICONERROR);
    }
    return;
  }

  if (deleteAfter && !g_app.archivePath.empty()) {
    const std::wstring arcPath = g_app.archivePath;
    g_app.archive.Close();
    g_app.archiveMode = false;
    ::DeleteFileW(arcPath.c_str());
    AppNavigateTo(util::DirName(arcPath));
  } else {
    AppRefreshView();
  }

  if (openAfter) ::ShellExecuteW(g_app.mainWnd, L"open", target.c_str(), nullptr, nullptr,
                                 SW_SHOWNORMAL);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      g_state = static_cast<DlgState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
      CreateControls(hwnd, reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)));
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd, &ps);
      PaintBanner(hwnd);
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;

    case WM_CTLCOLORBTN: {
      // 普通按钮交给系统默认绘制，分组框/复选/单选需要与对话框同色底
      HWND ctl = reinterpret_cast<HWND>(lParam);
      const LONG style = ctl ? ::GetWindowLongW(ctl, GWL_STYLE) : 0;
      const LONG type = style & BS_TYPEMASK;
      if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_OWNERDRAW) {
        break;
      }
      HDC dc = reinterpret_cast<HDC>(wParam);
      ::SetBkColor(dc, kDlgBg);
      static HBRUSH b = ::CreateSolidBrush(kDlgBg);
      return reinterpret_cast<LRESULT>(b);
    }

    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      ::SetBkColor(dc, kDlgBg);
      static HBRUSH b = ::CreateSolidBrush(kDlgBg);
      return reinterpret_cast<LRESULT>(b);
    }

    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      if (!g_state) break;
      switch (id) {
        case kIdBrowse:
          OnBrowse(hwnd);
          return 0;
        case kIdNewFolder:
          OnNewFolder(hwnd);
          return 0;
        case kIdPassword:
          OnPassword(hwnd);
          return 0;
        case kIdSaveDefault: {
          wchar_t buf[MAX_PATH * 2];
          ::GetWindowTextW(::GetDlgItem(hwnd, kIdTarget), buf, MAX_PATH * 2);
          g_app.settings.extractDir = buf;
          util::SaveSettings(g_app.settings);
          ::MessageBoxW(hwnd, L"已保存为默认设置。", L"MyZip", MB_OK | MB_ICONINFORMATION);
          return 0;
        }
        case kIdOk:
          OnOk(hwnd);
          return 0;
        case kIdCancel:
          ::DestroyWindow(hwnd);
          return 0;
        default:
          break;
      }
      break;
    }

    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      g_state = nullptr;
      return 0;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void ShowExtractDialog(HWND owner, const std::vector<int>& selected) {
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

  DlgState st;
  st.selected = selected;
  st.targetDir = DefaultTargetDir();

  const int dpi = g_app.dpi;
  auto px = [&](int v) { return ::MulDiv(v, dpi, 96); };

  RECT ownerRc{};
  if (owner) ::GetWindowRect(owner, &ownerRc);
  const int w = px(496);
  const int h = px(434);
  const int x = owner ? ownerRc.left + ((ownerRc.right - ownerRc.left) - w) / 2 : CW_USEDEFAULT;
  const int y = owner ? ownerRc.top + ((ownerRc.bottom - ownerRc.top) - h) / 2 : CW_USEDEFAULT;

  HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"解压",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, owner,
                               nullptr, g_app.instance, &st);
  if (!dlg) return;

  if (owner) ::EnableWindow(owner, FALSE);
  ::SetFocus(dlg);

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
}

}  // namespace ui
