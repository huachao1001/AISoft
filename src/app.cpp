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

#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
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
  struct DirAgg {
    bool seen = false;
    uint64_t size = 0;
    FILETIME mtime{};
    bool hasMtime = false;
  };
  std::vector<std::wstring> order;
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

  // 补上“上级目录”提示由 goUp 处理，不进列表
}

}  // namespace

void AppRefreshView() {
  if (g_app.archiveMode) {
    LoadArchiveItems();
    ui::UpdateMainTitle(g_app.mainWnd);
  } else {
    LoadDirectoryItems(g_app.curDir);
  }
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
    ::MessageBoxW(g_app.mainWnd, (L"路径不存在：\n" + dir).c_str(), L"MyZip", MB_OK | MB_ICONWARNING);
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

// 打开压缩包；加密文件名的包需要密码，首次失败时弹框询问后重试
static bool OpenArchiveWithPassword(arc::Archive& a, const std::wstring& path, HWND owner,
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

void AppOpenArchive(const std::wstring& path) {
  arc::OpenResult r;
  if (!OpenArchiveWithPassword(g_app.archive, path, g_app.mainWnd, r)) {
    if (r.error.empty()) return;  // 用户取消
    ::MessageBoxW(g_app.mainWnd, (L"无法打开压缩包：\n" + path + L"\n\n" + r.error).c_str(), L"MyZip",
                  MB_OK | MB_ICONERROR);
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
      if (OpenArchiveWithPassword(g_app.archive, target, g_app.mainWnd, r)) {
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
      if (OpenArchiveWithPassword(g_app.archive, target, g_app.mainWnd, r)) {
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
