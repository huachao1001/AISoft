#include "app.h"

#include "dlg_compress.h"
#include "dlg_extract.h"
#include "dlg_input.h"
#include "dlg_progress.h"
#include "ui.h"
#include "ui_list.h"
#include "ui_main.h"
#include "ui_tree.h"
#include "util.h"

#include <shellapi.h>

#include <map>

App g_app;

// ───────────────────────── 视图 ─────────────────────────

namespace {

std::wstring TypeNameFor(const std::wstring& name, bool isDir) {
  if (isDir) return L"文件夹";
  const std::wstring ext = util::FileExtension(name);
  if (ext == L".zip" || ext == L".7z" || ext == L".rar") return L"压缩文件";
  if (ext.empty()) return L"文件";
  std::wstring upper;
  for (size_t i = 1; i < ext.size(); ++i) upper.push_back(static_cast<wchar_t>(towupper(ext[i])));
  return upper + L" 文件";
}

void LoadDirectoryItems(const std::wstring& dir) {
  g_app.items.clear();
  WIN32_FIND_DATAW fd;
  HANDLE h = ::FindFirstFileW(util::JoinPath(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;

  const bool showHidden = false;
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
    if (!showHidden && (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
    if (!showHidden && (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)) continue;

    Item it;
    it.name = fd.cFileName;
    it.fullPath = util::JoinPath(dir, fd.cFileName);
    it.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    it.size = it.isDir ? 0 : (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    it.packSize = it.size;
    it.mtime = fd.ftLastWriteTime;
    it.typeName = TypeNameFor(it.name, it.isDir);
    it.iconIndex = ui::ShellIconIndex(it.fullPath, it.isDir);
    g_app.items.push_back(std::move(it));
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
}

void LoadArchiveItems() {
  g_app.items.clear();
  const auto& entries = g_app.archive.Entries();
  const std::wstring prefix = g_app.innerPath.empty() ? L"" : g_app.innerPath + L"\\";

  // 收集当前层级的直接子项，目录去重
  std::vector<Item> dirItems;
  std::map<std::wstring, size_t> dirIndex;

  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    std::wstring rel = e.path;
    if (!prefix.empty()) {
      if (rel.size() <= prefix.size()) continue;
      if (::CompareStringOrdinal(rel.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                                 static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) {
        continue;
      }
      rel = rel.substr(prefix.size());
    }
    if (rel.empty()) continue;

    const size_t slash = rel.find_first_of(L"\\/");
    if (slash != std::wstring::npos) {
      // 子目录
      const std::wstring dirName = rel.substr(0, slash);
      auto found = dirIndex.find(dirName);
      if (found == dirIndex.end()) {
        Item d;
        d.name = dirName;
        d.typeName = L"文件夹";
        d.isDir = true;
        d.iconIndex = ui::ShellIconIndex(L"", true);
        dirIndex[dirName] = dirItems.size();
        dirItems.push_back(std::move(d));
      }
      auto& agg = dirItems[dirIndex[dirName]];
      agg.size += e.size;
      if (e.hasMtime) agg.mtime = e.mtime;
      continue;
    }

    if (e.isDir) {
      // 空目录条目本身
      auto found = dirIndex.find(rel);
      if (found == dirIndex.end()) {
        Item d;
        d.name = rel;
        d.typeName = L"文件夹";
        d.isDir = true;
        d.mtime = e.mtime;
        d.iconIndex = ui::ShellIconIndex(L"", true);
        dirIndex[rel] = dirItems.size();
        dirItems.push_back(std::move(d));
      }
      continue;
    }

    Item it;
    it.name = rel;
    it.typeName = TypeNameFor(rel, false);
    it.size = e.size;
    it.packSize = e.packSize;
    it.crc = e.crc;
    it.hasCrc = e.hasCrc;
    it.mtime = e.mtime;
    it.method = e.method;
    it.encrypted = e.encrypted;
    it.entryIndex = static_cast<int>(i);
    it.iconIndex = ui::ShellIconIndex(L"x" + util::FileExtension(rel), false);
    g_app.items.push_back(std::move(it));
  }

  // 目录排在前面
  g_app.items.insert(g_app.items.begin(), dirItems.begin(), dirItems.end());
}

// 本程序内的窗口才能当模态 owner
HWND DialogOwner() {
  HWND h = g_app.mainWnd;
  // 最小化时被 owner 的弹窗会一起隐藏，退化为无 owner
  if (h && ::IsWindow(h) && !::IsIconic(h)) return h;
  return nullptr;
}

// 打开压缩包；头部加密的包需要密码，失败时弹框询问后重试
bool OpenArchiveWithPassword(arc::Archive& a, const std::wstring& path, HWND owner,
                             arc::OpenResult& r) {
  r = a.Open(path);
  if (r.ok) return true;

  // 头部加密（-mhe=on）的 7z 在无密码时连目录都读不出来，只能提示用户输入
  std::wstring pwd;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (!ui::PromptText(owner, L"输入密码", L"压缩包已加密，请输入密码：", pwd, true)) return false;
    r = a.Open(path, pwd);
    if (r.ok) return true;
  }
  return false;
}

}  // namespace

void AppRefreshView() {
  if (g_app.archiveMode) {
    LoadArchiveItems();
    ui::UpdateMainTitle(g_app.mainWnd);
  } else {
    LoadDirectoryItems(g_app.curDir);
  }
  // 树只在文件系统模式显示：进出压缩包都要重排，否则会留一条空栏
  ui::RelayoutMain();
  ui::RefreshFileList();
  ui::RefreshAddressBar(g_app.mainWnd);
  ui::RefreshStatusBar(g_app.mainWnd);
  if (!g_app.archiveMode) ui::SyncTreeToCurrentDir();
}

void AppNavigateTo(const std::wstring& dirIn) {
  std::wstring dir = dirIn;
  if (dir.empty()) return;
  // 归一化
  while (dir.size() > 3 && (dir.back() == L'\\')) dir.pop_back();
  if (!util::IsDirectory(dir)) {
    ::MessageBoxW(g_app.mainWnd, (L"路径不存在：\n" + dir).c_str(), L"AIZip", MB_OK | MB_ICONWARNING);
    return;
  }

  if (g_app.archiveMode) {
    g_app.archive.Close();
    g_app.archiveMode = false;
    g_app.innerPath.clear();
    g_app.archivePath.clear();
  }

  g_app.curDir = dir;
  g_app.settings.lastDir = dir;

  // 历史前进
  if (g_app.historyPos < 0 || g_app.historyPos >= static_cast<int>(g_app.history.size()) ||
      ::CompareStringOrdinal(g_app.history[g_app.historyPos].c_str(), -1, dir.c_str(), -1, TRUE) !=
          CSTR_EQUAL) {
    if (g_app.historyPos + 1 < static_cast<int>(g_app.history.size())) {
      g_app.history.erase(g_app.history.begin() + g_app.historyPos + 1, g_app.history.end());
    }
    g_app.history.push_back(dir);
    g_app.historyPos = static_cast<int>(g_app.history.size()) - 1;
  }

  AppRefreshView();
}

void AppOpenArchive(const std::wstring& path) {
  arc::OpenResult r;
  if (!OpenArchiveWithPassword(g_app.archive, path, DialogOwner(), r)) {
    if (r.error.empty()) return;  // 用户取消
    ::MessageBoxW(g_app.mainWnd, (L"无法打开压缩包：\n" + path + L"\n\n" + r.error).c_str(),
                  L"AIZip", MB_OK | MB_ICONERROR);
    return;
  }
  g_app.archiveMode = true;
  g_app.archivePath = path;
  g_app.innerPath.clear();

  if (g_app.historyPos + 1 < static_cast<int>(g_app.history.size())) {
    g_app.history.erase(g_app.history.begin() + g_app.historyPos + 1, g_app.history.end());
  }
  g_app.history.push_back(path);
  g_app.historyPos = static_cast<int>(g_app.history.size()) - 1;

  AppRefreshView();
}

void AppGoUp() {
  if (g_app.archiveMode) {
    if (!g_app.innerPath.empty()) {
      const size_t pos = g_app.innerPath.find_last_of(L'\\');
      g_app.innerPath = (pos == std::wstring::npos) ? L"" : g_app.innerPath.substr(0, pos);
      AppRefreshView();
      return;
    }
    // 压缩包根目录：回到所在文件夹
    AppNavigateTo(util::DirName(g_app.archivePath));
    return;
  }
  const std::wstring parent = util::DirName(g_app.curDir);
  if (!parent.empty() && parent != g_app.curDir) AppNavigateTo(parent);
}

void AppGoBack() {
  if (g_app.historyPos > 0) {
    --g_app.historyPos;
    const std::wstring target = g_app.history[g_app.historyPos];
    if (arc::Archive::IsArchiveFile(target) && util::PathExists(target)) {
      arc::OpenResult r;
      if (OpenArchiveWithPassword(g_app.archive, target, DialogOwner(), r)) {
        g_app.archiveMode = true;
        g_app.archivePath = target;
        g_app.innerPath.clear();
        AppRefreshView();
        return;
      }
    }
    const int save = g_app.historyPos;
    AppNavigateTo(target);
    g_app.historyPos = save;
  }
}

void AppGoForward() {
  if (g_app.historyPos + 1 < static_cast<int>(g_app.history.size())) {
    ++g_app.historyPos;
    const std::wstring target = g_app.history[g_app.historyPos];
    if (arc::Archive::IsArchiveFile(target) && util::PathExists(target)) {
      arc::OpenResult r;
      if (OpenArchiveWithPassword(g_app.archive, target, DialogOwner(), r)) {
        g_app.archiveMode = true;
        g_app.archivePath = target;
        g_app.innerPath.clear();
        AppRefreshView();
        return;
      }
    }
    const int save = g_app.historyPos;
    AppNavigateTo(target);
    g_app.historyPos = save;
  }
}

bool AppCanBack() { return g_app.historyPos > 0; }

bool AppCanForward() {
  return g_app.historyPos + 1 < static_cast<int>(g_app.history.size());
}

// ───────────────────────── 外部启动请求 ─────────────────────────

namespace {

bool EqualsNoCase(const std::wstring& a, const wchar_t* b) {
  return ::CompareStringOrdinal(a.c_str(), -1, b, -1, TRUE) == CSTR_EQUAL;
}

// 一键压缩：直接用源文件名生成 <名字>.zip，不弹对话框。
// 单个文件用主名，多选/文件夹用第一个的名字（与压缩对话框的默认命名一致）。
std::wstring QuickZipPath(const std::vector<std::wstring>& sources) {
  if (sources.empty()) return L"";
  const std::wstring& first = sources.front();
  const std::wstring dir = util::DirName(first);
  std::wstring base = util::IsDirectory(first) ? util::BaseName(first) : util::FileStem(first);
  if (base.empty()) base = L"archive";
  return util::JoinPath(dir, base + L".zip");
}

// 同名时自动让路成 "名字 (2).zip"，不覆盖用户已有的包
std::wstring QuickZipUniquePath(const std::wstring& path) {
  return util::PathExists(path) ? util::MakeUniquePath(path) : path;
}

}  // namespace

LaunchRequest ParseLaunchArgs() {
  LaunchRequest req;
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) return req;

  bool actionGiven = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (EqualsNoCase(a, L"-compress")) {
      req.action = LaunchRequest::Action::kCompress;
      actionGiven = true;
    } else if (EqualsNoCase(a, L"-compress-zip")) {
      req.action = LaunchRequest::Action::kQuickZip;
      actionGiven = true;
    } else if (EqualsNoCase(a, L"-extract")) {
      req.action = LaunchRequest::Action::kExtract;
      actionGiven = true;
    } else if (!a.empty() && a[0] != L'-') {
      req.paths.push_back(a);
    }
  }
  ::LocalFree(argv);

  req.explicitAction = actionGiven;
  // 没有动作开关时按后缀推断，这样资源管理器里「打开方式」选 AIZip 也能用
  if (!actionGiven && !req.paths.empty() && arc::Archive::IsArchiveFile(req.paths[0])) {
    req.action = LaunchRequest::Action::kExtract;
  }
  return req;
}

std::wstring SerializeLaunch(const LaunchRequest& req) {
  std::wstring s = L"-compress";
  if (req.action == LaunchRequest::Action::kExtract) {
    s = L"-extract";
  } else if (req.action == LaunchRequest::Action::kQuickZip) {
    s = L"-compress-zip";
  }
  for (const auto& p : req.paths) {
    s += L'\n';
    s += p;
  }
  return s;
}

LaunchRequest DeserializeLaunch(const std::wstring& packed) {
  LaunchRequest req;
  size_t pos = 0;
  bool actionGiven = false;
  while (pos <= packed.size()) {
    const size_t nl = packed.find(L'\n', pos);
    const std::wstring line =
        packed.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
    pos = (nl == std::wstring::npos) ? packed.size() + 1 : nl + 1;
    if (line.empty()) continue;
    if (EqualsNoCase(line, L"-compress")) {
      req.action = LaunchRequest::Action::kCompress;
      actionGiven = true;
    } else if (EqualsNoCase(line, L"-compress-zip")) {
      req.action = LaunchRequest::Action::kQuickZip;
      actionGiven = true;
    } else if (EqualsNoCase(line, L"-extract")) {
      req.action = LaunchRequest::Action::kExtract;
      actionGiven = true;
    } else {
      req.paths.push_back(line);
    }
  }
  req.explicitAction = actionGiven;
  if (!actionGiven && !req.paths.empty() && arc::Archive::IsArchiveFile(req.paths[0])) {
    req.action = LaunchRequest::Action::kExtract;
  }
  return req;
}

void AppDispatchLaunch(const LaunchRequest& req) {
  // 浏览模式（无动作开关）：在主窗口里定位到目标路径
  if (!req.explicitAction) {
    if (req.paths.empty()) return;
    const std::wstring& arg = req.paths[0];
    if (util::IsDirectory(arg)) {
      AppNavigateTo(arg);
    } else if (arc::Archive::IsArchiveFile(arg) && util::PathExists(arg)) {
      AppOpenArchive(arg);
    } else {
      AppNavigateTo(util::DirName(util::AbsolutePath(arg)));
    }
    return;
  }

  const HWND owner = DialogOwner();

  if (req.action == LaunchRequest::Action::kQuickZip) {
    if (req.paths.empty()) {
      ::MessageBoxW(owner, L"没有可压缩的文件。", L"AIZip", MB_OK | MB_ICONINFORMATION);
      return;
    }
    AppQuickZip(req.paths);
    return;
  }

  if (req.action == LaunchRequest::Action::kCompress) {
    if (req.paths.empty()) {
      ::MessageBoxW(owner, L"没有可压缩的文件。", L"AIZip", MB_OK | MB_ICONINFORMATION);
      return;
    }
    ui::ShowCompressDialog(owner, req.paths);
    return;
  }

  // ── 解压 ──
  if (req.paths.empty()) {
    ::MessageBoxW(owner, L"没有指定压缩包。", L"AIZip", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (req.paths.size() > 1) {
    ::MessageBoxW(owner, L"一次只能解压一个压缩包。", L"AIZip", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const std::wstring& path = req.paths[0];
  if (!arc::Archive::IsArchiveFile(path) || !util::PathExists(path)) {
    ::MessageBoxW(owner, (L"不是可识别的压缩包：\n" + path).c_str(), L"AIZip",
                  MB_OK | MB_ICONWARNING);
    return;
  }

  arc::OpenResult r;
  if (!OpenArchiveWithPassword(g_app.archive, path, owner, r)) {
    if (!r.error.empty()) {
      ::MessageBoxW(owner, (L"无法打开压缩包：\n" + path + L"\n\n" + r.error).c_str(), L"AIZip",
                    MB_OK | MB_ICONERROR);
    }
    return;
  }
  g_app.archivePath = path;
  // 解压对话框靠 archivePath 推默认目标目录，这里不进入浏览模式
  g_app.archiveMode = false;
  g_app.innerPath.clear();
  ui::ShowExtractDialog(owner);
}

void AppQuickZip(const std::vector<std::wstring>& paths) {
  const std::wstring target = QuickZipUniquePath(QuickZipPath(paths));
  if (target.empty()) return;

  arc::CompressOptions opts;
  opts.archivePath = target;
  opts.formatName = L"ZIP";
  opts.level = g_app.settings.compressLevel;
  opts.solid = g_app.settings.solid;

  std::wstring err;
  const bool ok =
      ui::ShowProgressDialog(DialogOwner(), L"正在压缩", [&](const arc::ProgressFn& prog) {
        return arc::Compress(paths, opts, prog, err);
      });

  if (!ok && !err.empty()) {
    ::MessageBoxW(DialogOwner(), (L"压缩失败：\n" + err).c_str(), L"AIZip", MB_OK | MB_ICONERROR);
  }
  if (ok && g_app.mainWnd) AppRefreshView();
}
