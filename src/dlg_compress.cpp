// 压缩对话框：对齐 HaoZip 的“压缩文件”窗口
#include "dlg_compress.h"

#include "app.h"
#include "archive.h"
#include "dlg_input.h"
#include "dlg_progress.h"
#include "sevenzip_loader.h"
#include "ui.h"
#include "util.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <string>
#include <vector>

namespace ui {
namespace {

constexpr const wchar_t* kClass = L"MyZipCompressDlg";

constexpr int kIdPath = 2001;
constexpr int kIdBrowse = 2002;
constexpr int kIdFormat7z = 2010;
constexpr int kIdFormatZip = 2011;
constexpr int kIdFormatOther = 2012;  // 第三个单选：无文字，标签就是旁边的下拉框
constexpr int kIdFormatCombo = 2013;  // 其余格式的下拉列表
constexpr int kIdLevel = 2021;
constexpr int kIdUpdate = 2022;
constexpr int kIdVolume = 2023;
constexpr int kIdVolumeUnit = 2024;
constexpr int kIdPassword = 2025;
constexpr int kIdComment = 2026;
constexpr int kIdOptSolid = 2040;
constexpr int kIdOptTest = 2041;
constexpr int kIdOptDelete = 2042;
constexpr int kIdOptSeparate = 2043;
constexpr int kIdOptSfx = 2044;
constexpr int kIdDefault = 2050;
constexpr int kIdOk = IDOK;
constexpr int kIdCancel = IDCANCEL;

struct DlgState {
  std::vector<std::wstring> sources;
  // 指向 z7::Formats() 的索引：0=ZIP 1=7Z，其余走下拉
  int formatIndex = 1;
  int levelIndex = 3;   // 指向 kLevelValues
  bool solid = true;
};

DlgState* g_state = nullptr;

const wchar_t* kLevelNames[] = {L"存储", L"最快", L"快速", L"标准", L"最大", L"极限"};
const int kLevelValues[] = {0, 1, 3, 5, 7, 9};
constexpr int kLevelCount = 6;

// 当前格式对应的扩展名
std::wstring ExtFor(int formatIndex) {
  const int n = z7::FormatCount();
  if (formatIndex < 0 || formatIndex >= n) return L".zip";
  return z7::Formats()[formatIndex].ext;
}

const wchar_t* NameFor(int formatIndex) {
  const int n = z7::FormatCount();
  if (formatIndex < 0 || formatIndex >= n) return L"ZIP";
  return z7::Formats()[formatIndex].name;
}

// 取不含归档扩展名的文件名（.tar.gz 这类两段扩展要整段剥掉）。
// 只返回末段名，调用方自己拼目录。
std::wstring StripArchiveExt(const std::wstring& path) {
  const std::wstring base = util::BaseName(path);
  for (int i = 0; i < z7::FormatCount(); ++i) {
    const std::wstring ext = z7::Formats()[i].ext;
    if (base.size() > ext.size() &&
        ::CompareStringOrdinal(base.c_str() + (base.size() - ext.size()),
                               static_cast<int>(ext.size()), ext.c_str(),
                               static_cast<int>(ext.size()), TRUE) == CSTR_EQUAL) {
      return base.substr(0, base.size() - ext.size());
    }
  }
  const size_t dot = base.find_last_of(L'.');
  return (dot == std::wstring::npos) ? base : base.substr(0, dot);
}

std::wstring DefaultArchivePath(const DlgState& st) {
  if (st.sources.empty()) return L"";
  const std::wstring first = st.sources.front();
  const std::wstring dir = util::DirName(first);
  std::wstring base = util::IsDirectory(first) ? util::BaseName(first) : util::FileStem(first);
  if (base.empty()) base = L"archive";
  return util::JoinPath(dir, base + ExtFor(st.formatIndex));
}

void CreateControls(HWND hwnd) {
  auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
                  int id) {
    return CreateDlgItem(hwnd, cls, text, style, x, y, w, h, id);
  };

  make(L"STATIC", L"压缩文件名(F)", SS_LEFT, 12, 12, 160, 16, 0);
  make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 12, 30, 330, 22, kIdPath);
  make(L"BUTTON", L"浏览(B)...", BS_PUSHBUTTON | WS_TABSTOP, 348, 29, 88, 24, kIdBrowse);

  make(L"STATIC", L"压缩文件格式", SS_LEFT, 12, 62, 160, 16, 0);
  // 与 HaoZip 一致：○ ZIP ○ 7Z ○ [格式▾]，第三个单选没有文字，下拉框充当它的标签
  make(L"BUTTON", L"ZIP", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 12, 82, 46, 18, kIdFormatZip);
  make(L"BUTTON", L"7Z", BS_AUTORADIOBUTTON | WS_TABSTOP, 62, 82, 40, 18, kIdFormat7z);
  make(L"BUTTON", L"", BS_AUTORADIOBUTTON | WS_TABSTOP, 104, 82, 16, 18, kIdFormatOther);
  // 宽度止于 204：右侧「压缩选项」分组框从 216 开始，越界会压住它
  HWND fmtCombo = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 122, 81,
                       82, 220, kIdFormatCombo);
  for (int i = 2; i < z7::FormatCount(); ++i) {
    ::SendMessageW(fmtCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(z7::Formats()[i].name));
  }
  ::SendMessageW(fmtCombo, CB_SETCURSEL, 0, 0);

  make(L"STATIC", L"压缩方式(M)", SS_LEFT, 12, 110, 160, 16, 0);
  HWND level = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 12, 128, 190, 200,
                    kIdLevel);
  for (int i = 0; i < kLevelCount; ++i) {
    ::SendMessageW(level, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kLevelNames[i]));
  }
  ::SendMessageW(level, CB_SETCURSEL, 3, 0);

  make(L"STATIC", L"更新方式(U)", SS_LEFT, 12, 158, 160, 16, 0);
  HWND update = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 12, 176, 190, 200,
                     kIdUpdate);
  for (const wchar_t* s : {L"添加并替换文件", L"添加并更新文件", L"仅更新已存在的文件",
                           L"同步文件夹内容"}) {
    ::SendMessageW(update, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
  }
  ::SendMessageW(update, CB_SETCURSEL, 0, 0);

  make(L"STATIC", L"压缩分卷大小设置(V)", SS_LEFT, 12, 206, 200, 16, 0);
  // 同左栏约束：输入框与单位下拉合计必须止于 204
  make(L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP | CBS_AUTOHSCROLL, 12, 224, 138,
       200, kIdVolume);
  HWND volUnit = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 156, 224, 48,
                      200, kIdVolumeUnit);
  for (const wchar_t* s : {L"MB", L"GB", L"KB"}) {
    ::SendMessageW(volUnit, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
  }
  ::SendMessageW(volUnit, CB_SETCURSEL, 0, 0);

  // 右侧压缩选项
  make(L"BUTTON", L"压缩选项", BS_GROUPBOX, 216, 62, 220, 190, 0);
  make(L"BUTTON", L"压缩后删除源文件(L)", BS_AUTOCHECKBOX | WS_TABSTOP, 226, 82, 200, 18,
       kIdOptDelete);
  make(L"BUTTON", L"压缩后测试压缩文件(E)", BS_AUTOCHECKBOX | WS_TABSTOP, 226, 104, 200, 18,
       kIdOptTest);
  HWND solid = make(L"BUTTON", L"创建固实压缩文件(S)(仅 7Z)", BS_AUTOCHECKBOX | WS_TABSTOP, 226,
                    126, 200, 18, kIdOptSolid);
  ::SendMessageW(solid, BM_SETCHECK, BST_CHECKED, 0);
  make(L"BUTTON", L"压缩每个选中项到单独的压缩文件", BS_AUTOCHECKBOX | WS_TABSTOP, 226, 148, 200,
       18, kIdOptSeparate);
  make(L"BUTTON", L"创建自解压格式压缩文件(X)", BS_AUTOCHECKBOX | WS_DISABLED, 226, 170, 200, 18,
       kIdOptSfx);

  make(L"BUTTON", L"密码(P)...", BS_PUSHBUTTON | WS_TABSTOP, 12, 258, 88, 24, kIdPassword);
  make(L"BUTTON", L"注释(M)...", BS_PUSHBUTTON | WS_TABSTOP, 106, 258, 88, 24, kIdComment);

  make(L"BUTTON", L"保存为默认(S)", BS_PUSHBUTTON | WS_TABSTOP, 12, 314, 110, 24, kIdDefault);
  make(L"BUTTON", L"确定(O)", BS_DEFPUSHBUTTON | WS_TABSTOP, 290, 314, 72, 26, kIdOk);
  make(L"BUTTON", L"取消(C)", BS_PUSHBUTTON | WS_TABSTOP, 368, 314, 72, 26, kIdCancel);

  for (HWND c = ::GetWindow(hwnd, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT)) {
    ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.ui), TRUE);
  }
}

// 三个互斥单选：ZIP、7Z、以及“其他格式”（无文字，由旁边下拉框承载）。
// 选中下拉框里某项等于选中第三个单选。
void SetFormatSelection(HWND hwnd, int formatIndex) {
  HWND zip = ::GetDlgItem(hwnd, kIdFormatZip);
  HWND sevenZ = ::GetDlgItem(hwnd, kIdFormat7z);
  HWND other = ::GetDlgItem(hwnd, kIdFormatOther);
  HWND combo = ::GetDlgItem(hwnd, kIdFormatCombo);
  if (zip) ::SendMessageW(zip, BM_SETCHECK, formatIndex == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
  if (sevenZ) {
    ::SendMessageW(sevenZ, BM_SETCHECK, formatIndex == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  const bool isOther = formatIndex >= 2;
  if (other) ::SendMessageW(other, BM_SETCHECK, isOther ? BST_CHECKED : BST_UNCHECKED, 0);
  if (combo) {
    if (isOther) {
      ::SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(formatIndex - 2), 0);
      ::EnableWindow(combo, TRUE);
    } else {
      ::SendMessageW(combo, CB_SETCURSEL, 0, 0);
      ::EnableWindow(combo, FALSE);
    }
  }
}

// 切换格式后同步扩展名、固实与档位可用性
void SyncFormatUi(HWND hwnd, int formatIndex) {
  const int n = z7::FormatCount();
  const bool valid = formatIndex >= 0 && formatIndex < n;
  const z7::FormatInfo* fi = valid ? &z7::Formats()[formatIndex] : nullptr;

  // 只有 7Z 支持固实；TAR/WIM 只有存储档
  if (HWND solid = ::GetDlgItem(hwnd, kIdOptSolid)) {
    ::EnableWindow(solid, fi && fi->solidCapable);
  }
  if (HWND level = ::GetDlgItem(hwnd, kIdLevel)) {
    ::EnableWindow(level, !fi || fi->maxLevel > 0);
  }

  HWND edit = ::GetDlgItem(hwnd, kIdPath);
  if (!edit) return;
  wchar_t buf[MAX_PATH * 2];
  ::GetWindowTextW(edit, buf, MAX_PATH * 2);
  std::wstring cur = buf;
  if (cur.empty()) return;
  // 要把已有的归档扩展名整段剥掉再拼新的，否则连续切换会叠成 xxx.tar.tar.xz
  const std::wstring stem = StripArchiveExt(cur);
  const std::wstring dir = util::DirName(cur);
  if (stem.empty()) return;
  const std::wstring next = util::JoinPath(dir, stem + ExtFor(formatIndex));
  ::SetWindowTextW(edit, next.c_str());
}

void OnBrowse(HWND hwnd, DlgState& st) {
  wchar_t file[MAX_PATH] = L"";
  ::StringCchCopyW(file, MAX_PATH, DefaultArchivePath(st).c_str());

  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd;
  ofn.lpstrFilter =
      L"ZIP 压缩文件\0*.zip\07Z 压缩文件\0*.7z\0TAR.GZ 压缩文件\0*.tar.gz\0TAR.BZ2 压缩文件\0*.tar.bz2\0"
      L"TAR.XZ 压缩文件\0*.tar.xz\0GZIP 文件\0*.gz\0BZIP2 文件\0*.bz2\0XZ 文件\0*.xz\0"
      L"TAR 归档\0*.tar\0WIM 映像\0*.wim\0所有文件\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = L"选择压缩文件名";
  ofn.lpstrDefExt = ExtFor(st.formatIndex).c_str() + 1;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
  if (::GetSaveFileNameW(&ofn)) {
    ::SetWindowTextW(::GetDlgItem(hwnd, kIdPath), file);
  }
}

std::wstring g_password;

void OnPassword(HWND hwnd) {
  std::wstring pwd = g_password;
  if (PromptText(hwnd, L"设置密码", L"密码（留空表示不加密）：", pwd, true)) {
    g_password = pwd;
  }
}

// 读取界面状态。真正的来源是三个互斥单选；第三个单选选中的格式看下拉框
void ReadState(HWND hwnd, DlgState& st) {
  if (::SendMessageW(::GetDlgItem(hwnd, kIdFormatZip), BM_GETCHECK, 0, 0) == BST_CHECKED) {
    st.formatIndex = 0;
  } else if (::SendMessageW(::GetDlgItem(hwnd, kIdFormat7z), BM_GETCHECK, 0, 0) == BST_CHECKED) {
    st.formatIndex = 1;
  } else {
    const int sel =
        static_cast<int>(::SendMessageW(::GetDlgItem(hwnd, kIdFormatCombo), CB_GETCURSEL, 0, 0));
    st.formatIndex = 2 + (sel < 0 ? 0 : sel);
  }
  const int levelSel =
      static_cast<int>(::SendMessageW(::GetDlgItem(hwnd, kIdLevel), CB_GETCURSEL, 0, 0));
  st.levelIndex = (levelSel >= 0 && levelSel < kLevelCount) ? levelSel : 3;
  st.solid = ::SendMessageW(::GetDlgItem(hwnd, kIdOptSolid), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void OnOk(HWND hwnd, DlgState& st) {
  wchar_t pathBuf[MAX_PATH * 2];
  ::GetWindowTextW(::GetDlgItem(hwnd, kIdPath), pathBuf, MAX_PATH * 2);
  std::wstring outPath = pathBuf;
  if (outPath.empty()) {
    ::MessageBoxW(hwnd, L"请填写压缩文件名。", L"MyZip", MB_OK | MB_ICONINFORMATION);
    return;
  }
  outPath = util::AbsolutePath(outPath);
  const std::wstring dir = util::DirName(outPath);
  if (!dir.empty() && !util::EnsureDirectory(dir)) {
    ::MessageBoxW(hwnd, L"无法创建目标目录。", L"MyZip", MB_OK | MB_ICONERROR);
    return;
  }
  if (util::PathExists(outPath)) {
    if (::MessageBoxW(hwnd, L"文件已存在，是否覆盖？", L"MyZip",
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
      return;
    }
  }

  ReadState(hwnd, st);
  const bool testAfter =
      ::SendMessageW(::GetDlgItem(hwnd, kIdOptTest), BM_GETCHECK, 0, 0) == BST_CHECKED;
  const bool deleteAfter =
      ::SendMessageW(::GetDlgItem(hwnd, kIdOptDelete), BM_GETCHECK, 0, 0) == BST_CHECKED;
  const bool separate =
      ::SendMessageW(::GetDlgItem(hwnd, kIdOptSeparate), BM_GETCHECK, 0, 0) == BST_CHECKED;

  g_app.settings.compressFormat = NameFor(st.formatIndex);
  g_app.settings.compressLevel = kLevelValues[st.levelIndex];
  g_app.settings.solid = st.solid;

  arc::CompressOptions opts;
  opts.level = kLevelValues[st.levelIndex];
  opts.solid = st.solid;
  opts.password = g_password;
  opts.formatName = NameFor(st.formatIndex);

  // TAR/WIM 只有存储档，内核不认 x 参数
  const z7::FormatInfo& fi = z7::Formats()[st.formatIndex];
  if (fi.maxLevel == 0) opts.level = 0;

  // 分离模式：每个源各打一个包
  std::vector<std::pair<std::wstring, std::vector<std::wstring>>> jobs;
  if (separate) {
    for (const auto& src : st.sources) {
      const std::wstring base = util::IsDirectory(src) ? util::BaseName(src) : util::FileStem(src);
      jobs.emplace_back(util::JoinPath(dir, base + ExtFor(st.formatIndex)),
                        std::vector<std::wstring>{src});
    }
  } else {
    jobs.emplace_back(outPath, st.sources);
  }

  // GZIP/BZIP2/XZ 只收单文件：分离模式天然满足，合并模式多文件/目录要提前拦下
  if (!separate && fi.singleFile) {
    std::vector<util::SourceItem> probe;
    util::CollectSources(st.sources, probe);
    if (probe.size() != 1) {
      ::MessageBoxW(hwnd,
                    (std::wstring(L"“") + fi.name +
                     L"”格式一次只能压缩一个文件。\n\n"
                     L"要打包多个文件或整个文件夹，请改用「TAR." +
                     (::IsEqualGUID(fi.clsId, z7::kClsidGZip) ? std::wstring(L"GZ")
                                                              : (::IsEqualGUID(fi.clsId, z7::kClsidBZip2)
                                                                     ? std::wstring(L"BZ2")
                                                                     : std::wstring(L"XZ"))) +
                     L"」，或勾选「压缩每个选中项到单独的压缩文件」。")
                        .c_str(),
                    L"MyZip", MB_OK | MB_ICONINFORMATION);
      return;
    }
  }

  ::DestroyWindow(hwnd);

  std::wstring err;
  const bool ok = ShowProgressDialog(g_app.mainWnd, L"正在压缩", [&](const arc::ProgressFn& prog) {
    for (auto& job : jobs) {
      arc::CompressOptions o = opts;
      o.archivePath = job.first;
      if (!arc::Compress(job.second, o, prog, err)) return false;
    }
    return true;
  });

  if (!ok) {
    if (!err.empty()) {
      ::MessageBoxW(g_app.mainWnd, (L"压缩失败：\n" + err).c_str(), L"MyZip", MB_OK | MB_ICONERROR);
    }
    return;
  }

  // 压缩后删除源文件
  if (deleteAfter) {
    for (const auto& src : st.sources) {
      if (util::IsDirectory(src)) {
        std::wstring dbl = src;
        dbl.push_back(L'\0');
        dbl.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = dbl.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        ::SHFileOperationW(&op);
      } else {
        ::DeleteFileW(src.c_str());
      }
    }
  }

  AppRefreshView();

  if (testAfter) {
    // 简单校验：重新打开并枚举
    arc::Archive check;
    const auto r = check.Open(outPath);
    if (!r.ok) {
      ::MessageBoxW(g_app.mainWnd, L"压缩包测试失败。", L"MyZip", MB_OK | MB_ICONWARNING);
    }
  }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE: {
      g_state = static_cast<DlgState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
      CreateControls(hwnd);
      ::SetWindowTextW(::GetDlgItem(hwnd, kIdPath), DefaultArchivePath(*g_state).c_str());

      // 恢复上次格式：前两项走单选，其余走下拉
      int fmt = g_state->formatIndex;
      if (fmt < 0 || fmt >= z7::FormatCount()) fmt = 0;
      SetFormatSelection(hwnd, fmt);
      {
        const z7::FormatInfo& fi = z7::Formats()[fmt];
        ::EnableWindow(::GetDlgItem(hwnd, kIdOptSolid), fi.solidCapable);
        ::EnableWindow(::GetDlgItem(hwnd, kIdLevel), fi.maxLevel > 0);
      }

      const int lv = (g_state->levelIndex >= 0 && g_state->levelIndex < kLevelCount)
                         ? g_state->levelIndex
                         : 3;
      ::SendMessageW(::GetDlgItem(hwnd, kIdLevel), CB_SETCURSEL, lv, 0);
      return 0;
    }

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
          OnBrowse(hwnd, *g_state);
          return 0;
        case kIdPassword:
          OnPassword(hwnd);
          return 0;
        case kIdComment:
          ::MessageBoxW(hwnd, L"压缩包注释暂未支持。", L"MyZip", MB_OK | MB_ICONINFORMATION);
          return 0;
        case kIdOk:
          OnOk(hwnd, *g_state);
          return 0;
        case kIdCancel:
          ::DestroyWindow(hwnd);
          return 0;
        case kIdFormatZip:
        case kIdFormat7z:
        case kIdFormatOther: {
          // 三个互斥单选，第三个选中时用下拉框里的当前项
          if (id == kIdFormatZip) {
            g_state->formatIndex = 0;
          } else if (id == kIdFormat7z) {
            g_state->formatIndex = 1;
          } else {
            const int sel = static_cast<int>(
                ::SendMessageW(::GetDlgItem(hwnd, kIdFormatCombo), CB_GETCURSEL, 0, 0));
            g_state->formatIndex = 2 + (sel < 0 ? 0 : sel);
          }
          SetFormatSelection(hwnd, g_state->formatIndex);
          SyncFormatUi(hwnd, g_state->formatIndex);
          return 0;
        }
        case kIdFormatCombo:
          if (HIWORD(wParam) == CBN_SELCHANGE) {
            const int sel = static_cast<int>(
                ::SendMessageW(::GetDlgItem(hwnd, kIdFormatCombo), CB_GETCURSEL, 0, 0));
            if (sel >= 0) {
              g_state->formatIndex = 2 + sel;
              SetFormatSelection(hwnd, g_state->formatIndex);
              SyncFormatUi(hwnd, g_state->formatIndex);
            }
          }
          return 0;
        case kIdDefault:
          ReadState(hwnd, *g_state);
          g_app.settings.compressFormat = NameFor(g_state->formatIndex);
          g_app.settings.compressLevel = kLevelValues[g_state->levelIndex];
          g_app.settings.solid = g_state->solid;
          util::SaveSettings(g_app.settings);
          ::MessageBoxW(hwnd, L"已保存为默认设置。", L"MyZip", MB_OK | MB_ICONINFORMATION);
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

void ShowCompressDialog(HWND owner, const std::vector<std::wstring>& sourcesIn) {
  static bool registered = false;
  if (!registered) {
    registered = RegisterWindowClass(kClass, Proc, g_app.instance, CS_HREDRAW | CS_VREDRAW,
                                     reinterpret_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
  }

  DlgState st;
  st.sources = sourcesIn;
  if (st.sources.empty() && !g_app.archiveMode) st.sources.push_back(g_app.curDir);
  if (st.sources.empty()) {
    ::MessageBoxW(owner, L"没有可压缩的内容。", L"MyZip", MB_OK | MB_ICONINFORMATION);
    return;
  }
  // 从设置里恢复格式名，按名查索引（顺序变了也不会串）
  st.formatIndex = z7::FormatIndexByName(g_app.settings.compressFormat.c_str());
  if (st.formatIndex < 0) st.formatIndex = 0;

  // 把 settings 的 x 值反查成下拉索引
  st.levelIndex = 3;
  for (int i = 0; i < kLevelCount; ++i) {
    if (kLevelValues[i] == g_app.settings.compressLevel) {
      st.levelIndex = i;
      break;
    }
  }
  st.solid = g_app.settings.solid;

  const int w = g_app.px(456);
  const int h = g_app.px(384);
  const RECT dr = DialogRect(owner, w, h);

  HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"压缩文件",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, dr.left, dr.top, w,
                               h, owner, nullptr, g_app.instance, &st);
  if (!dlg) return;

  ::SetFocus(dlg);
  RunModalDialog(dlg, owner);
}

}  // namespace ui
