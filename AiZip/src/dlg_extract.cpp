// 解压对话框
#include "dlg_extract.h"

#include "app.h"
#include "archive.h"
#include "dlg_input.h"
#include "dlg_progress.h"
#include "ui.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <string>
#include <vector>

namespace ui {
namespace {

constexpr const wchar_t* kClass = L"AIZipExtractDlg";

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
  std::vector<int> selected;  // g_app.items 下标；空表示整包
  std::wstring prefix;        // 包内目录前缀；非空时只解压其下成员
  std::wstring targetDir;
  int updateIndex = 0;
  int overwriteIndex = 1;  // 默认覆盖
  bool openAfter = true;
  bool deleteAfter = false;
  std::wstring password;
};

DlgState* g_state = nullptr;

std::wstring DefaultTargetDir() {
  if (!g_app.archivePath.empty()) {
    const std::wstring dir = util::DirName(g_app.archivePath);
    const std::wstring stem = util::FileStem(g_app.archivePath);
    return util::JoinPath(dir, stem);
  }
  if (!g_app.settings.extractDir.empty()) return g_app.settings.extractDir;
  return g_app.curDir;
}

void CreateControls(HWND hwnd) {
  auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
                  int id) {
    return CreateDlgItem(hwnd, cls, text, style, x, y, w, h, id);
  };

  make(L"STATIC", L"目标路径(T)", SS_LEFT, 12, 12, 160, 16, 0);
  make(L"EDIT", g_state->targetDir.c_str(), WS_BORDER | ES_AUTOHSCROLL, 12, 30, 380, 22, kIdTarget);
  make(L"BUTTON", L"浏览(B)...", BS_PUSHBUTTON | WS_TABSTOP, 398, 29, 104, 24, kIdBrowse);
  make(L"BUTTON", L"新建文件夹(N)", BS_PUSHBUTTON | WS_TABSTOP, 398, 57, 104, 24, kIdNewFolder);

  // 更新方式
  make(L"BUTTON", L"更新方式", BS_GROUPBOX, 12, 72, 380, 84, kIdGroupUpdate);
  make(L"BUTTON", L"解压并替换文件(R)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 22, 92, 350,
       18, kIdUpdate1);
  make(L"BUTTON", L"解压并更新文件(U)", BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 112, 350, 18,
       kIdUpdate2);
  make(L"BUTTON", L"仅更新已存在的文件(E)", BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 132, 350, 18,
       kIdUpdate3);
  ::SendMessageW(::GetDlgItem(hwnd, kIdUpdate1), BM_SETCHECK, BST_CHECKED, 0);

  // 覆盖方式
  make(L"BUTTON", L"覆盖方式", BS_GROUPBOX, 12, 162, 380, 100, kIdGroupOver);
  make(L"BUTTON", L"覆盖前询问(A)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 22, 182, 350, 18,
       kIdOver1);
  make(L"BUTTON", L"直接覆盖，不询问(W)", BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 202, 350, 18,
       kIdOver2);
  make(L"BUTTON", L"跳过已存在的文件(S)", BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 222, 350, 18,
       kIdOver3);
  make(L"BUTTON", L"自动重命名已存在的文件(R)", BS_AUTORADIOBUTTON | WS_TABSTOP, 22, 242, 350, 18,
       kIdOver4);
  ::SendMessageW(::GetDlgItem(hwnd, kIdOver2), BM_SETCHECK, BST_CHECKED, 0);

  // 其他选项
  make(L"BUTTON", L"其他选项", BS_GROUPBOX, 12, 268, 380, 82, kIdGroupOther);
  HWND openChk = make(L"BUTTON", L"解压完成后打开目标文件夹(O)", BS_AUTOCHECKBOX | WS_TABSTOP, 22,
                      284, 350, 18, kIdOptOpen);
  ::SendMessageW(openChk, BM_SETCHECK, BST_CHECKED, 0);
  make(L"BUTTON", L"解压完成后删除压缩包(D)", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 304, 350, 18,
       kIdOptDelete);
  make(L"BUTTON", L"密码(P)...", BS_PUSHBUTTON | WS_TABSTOP, 22, 322, 84, 22, kIdPassword);

  make(L"BUTTON", L"保存为默认(S)", BS_PUSHBUTTON | WS_TABSTOP, 12, 362, 110, 24, kIdSaveDefault);
  make(L"BUTTON", L"确定(O)", BS_DEFPUSHBUTTON | WS_TABSTOP, 320, 360, 68, 26, kIdOk);
  make(L"BUTTON", L"取消(C)", BS_PUSHBUTTON | WS_TABSTOP, 394, 360, 68, 26, kIdCancel);

  for (HWND c = ::GetWindow(hwnd, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT)) {
    ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(DialogFont()), TRUE);
  }
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
    ::MessageBoxW(hwnd, L"无法创建文件夹。", L"AIZip", MB_OK | MB_ICONERROR);
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

// p 是否等于 prefix 或位于 prefix 目录之下；避免 "probe" 误匹配 "probe2"
bool IsUnderDir(const std::wstring& p, const std::wstring& prefix) {
  if (prefix.empty()) return true;
  if (p.size() < prefix.size()) return false;
  if (::CompareStringOrdinal(p.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                             static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) {
    return false;
  }
  return p.size() == prefix.size() || p[prefix.size()] == L'\\' || p[prefix.size()] == L'/';
}

void OnOk(HWND hwnd) {
  wchar_t buf[MAX_PATH * 2];
  ::GetWindowTextW(::GetDlgItem(hwnd, kIdTarget), buf, MAX_PATH * 2);
  std::wstring target = buf;
  if (target.empty()) {
    ::MessageBoxW(hwnd, L"请填写解压目标路径。", L"AIZip", MB_OK | MB_ICONINFORMATION);
    return;
  }
  target = util::AbsolutePath(target);
  if (!util::EnsureDirectory(target)) {
    ::MessageBoxW(hwnd, L"无法创建目标文件夹。", L"AIZip", MB_OK | MB_ICONERROR);
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

  // 收集要解压的成员下标；既没有选中项也没有目录前缀时就是整包
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
  if (g_state->selected.empty() && !g_state->prefix.empty()) {
    // 左侧树选中的包内目录：按路径前缀收集
    for (size_t i = 0; i < g_app.archive.Entries().size(); ++i) {
      if (IsUnderDir(g_app.archive.Entries()[i].path, g_state->prefix)) {
        indices.push_back(static_cast<uint32_t>(i));
      }
    }
  }

  g_app.settings.extractDir = target;
  g_app.settings.openAfterExtract = openAfter;

  // 先记下父窗口：下面要销毁对话框，进度框不能再拿它当 owner
  HWND owner = ::GetParent(hwnd);
  ::DestroyWindow(hwnd);

  std::wstring err;
  const bool ok = ShowProgressDialog(owner, L"正在解压", [&](const arc::ProgressFn& prog) {
    return g_app.archive.Extract(
        indices, opts, prog,
        [owner](const std::wstring& path) -> int {
          int result = 1;
          RunOnUiThread([&]() {
            const std::wstring msg = L"文件已存在：\n\n" + path + L"\n\n是否覆盖？";
            const int r = ::MessageBoxW(owner, msg.c_str(), L"AIZip", MB_YESNO | MB_ICONWARNING);
            result = (r == IDYES) ? 1 : 0;
          });
          return result;
        },
        [owner](std::wstring& out) -> bool {
          bool accepted = false;
          RunOnUiThread([&]() {
            std::wstring pwd;
            accepted = PromptText(owner, L"输入密码", L"压缩包已加密，请输入密码：", pwd, true);
            if (accepted) out = pwd;
          });
          return accepted;
        },
        err);
  });

  if (!ok) {
    if (!err.empty()) {
      ::MessageBoxW(owner, (L"解压失败：\n" + err).c_str(), L"AIZip", MB_OK | MB_ICONERROR);
    }
    return;
  }

  if (deleteAfter && !g_app.archivePath.empty()) {
    const std::wstring arcPath = g_app.archivePath;
    g_app.archive.Close();
    ::DeleteFileW(arcPath.c_str());
    if (g_app.archiveMode) {
      g_app.archiveMode = false;
      g_app.archivePath.clear();
      AppNavigateTo(util::DirName(arcPath));
    }
  } else if (g_app.mainWnd) {
    AppRefreshView();
  }

  if (openAfter) {
    ::ShellExecuteW(owner, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      g_state = static_cast<DlgState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
      CreateControls(hwnd);
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      ::BeginPaint(hwnd, &ps);
      PaintDialogBackground(hwnd);
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
      HBRUSH b = DialogCtlBrush(msg, reinterpret_cast<HWND>(lParam));
      if (!b) break;  // 普通按钮交给 DefWindowProc
      ::SetBkColor(reinterpret_cast<HDC>(wParam), kDialogBg);
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
          ::MessageBoxW(hwnd, L"已保存为默认设置。", L"AIZip", MB_OK | MB_ICONINFORMATION);
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

void ShowExtractDialog(HWND owner, const std::vector<int>& selected, const std::wstring& prefix) {
  static bool registered = false;
  if (!registered) {
    registered = RegisterWindowClass(kClass, Proc, g_app.instance, CS_HREDRAW | CS_VREDRAW,
                                     reinterpret_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
  }

  DlgState st;
  st.selected = selected;
  st.prefix = prefix;
  st.targetDir = DefaultTargetDir();

  const int w = g_app.px(512);
  const int h = g_app.px(448);
  const RECT dr = DialogRect(owner, w, h);

  HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"解压",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, dr.left, dr.top, w,
                               h, owner, nullptr, g_app.instance, &st);
  if (!dlg) return;

  ::SetFocus(dlg);
  RunModalDialog(dlg, owner);
}

}  // namespace ui
