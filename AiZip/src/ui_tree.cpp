// 左侧文件夹树：业务适配层，视图与交互由 widgets::TreeView 负责
#include "ui_tree.h"

#include "app.h"
#include "ui.h"
#include "ui_main.h"
#include "util.h"
#include "AUI/theme.h"
#include "AUI/tree_view.h"

#include <shlobj.h>

#include <algorithm>
#include <memory>

using namespace AUI;

namespace ui {
namespace {

std::unique_ptr<TreeView> g_view;

// 枚举目录下的一级子目录（跳过隐藏与系统目录）
std::vector<std::wstring> SubDirs(const std::wstring& dir) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = ::FindFirstFileW(util::JoinPath(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
                                    (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
      continue;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    // 用户目录下大量 .xxx 会把可视区占满
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;
    if (fd.cFileName[0] == L'.') continue;
    out.emplace_back(fd.cFileName);
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
  std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
    return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  return out;
}

std::unique_ptr<TreeNode> MakeNode(const std::wstring& label, const std::wstring& path, int depth,
                                   TreeNode* parent, IconId icon) {
  auto n = std::make_unique<TreeNode>();
  n->label = label;
  n->path = path;
  n->depth = depth;
  n->parent = parent;
  n->icon = icon;
  return n;
}

// 节点子项：压缩包内从条目里取同层目录，文件系统则枚举磁盘
std::vector<std::unique_ptr<TreeNode>> LoadChildren(TreeNode* n) {
  std::vector<std::unique_ptr<TreeNode>> out;
  if (!n) return out;

  if (g_app.archiveMode) {
    const std::wstring prefix = n->path.empty() ? L"" : n->path + L"\\";
    std::vector<std::wstring> names;
    for (const auto& e : g_app.archive.Entries()) {
      if (e.path.size() <= prefix.size()) continue;
      if (!prefix.empty() &&
          ::CompareStringOrdinal(e.path.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                                 static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) {
        continue;
      }
      const std::wstring rel = e.path.substr(prefix.size());
      const size_t slash = rel.find_first_of(L"\\/");
      // 只有还带子路径的条目（或其本身是目录）才算树节点，文件不进树
      if (slash == std::wstring::npos && !e.isDir) continue;
      const std::wstring name = (slash == std::wstring::npos) ? rel : rel.substr(0, slash);
      if (name.empty()) continue;
      if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
      return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    for (const auto& name : names) {
      const std::wstring inner = n->path.empty() ? name : n->path + L"\\" + name;
      out.push_back(MakeNode(name, inner, n->depth + 1, n, IconId::kFolder));
    }
    return out;
  }

  for (const auto& d : SubDirs(n->path)) {
    out.push_back(MakeNode(d, util::JoinPath(n->path, d), n->depth + 1, n, IconId::kFolder));
  }
  return out;
}

// 根：压缩包模式只有包本身；文件系统放桌面、盘符、用户目录
std::unique_ptr<TreeNode> BuildRoot() {
  auto root = std::make_unique<TreeNode>();
  root->depth = -1;
  root->loaded = true;
  root->expanded = true;

  if (g_app.archiveMode) {
    auto n = MakeNode(util::BaseName(g_app.archivePath) + L"（根目录）", L"", 0, root.get(),
                      IconId::kHome);
    n->isRoot = true;
    root->children.push_back(std::move(n));
    return root;
  }

  wchar_t desktop[MAX_PATH] = {};
  if (::SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop) == S_OK) {
    auto n = MakeNode(L"桌面", desktop, 0, root.get(), IconId::kHome);
    n->isRoot = true;
    root->children.push_back(std::move(n));
  }
  const DWORD drives = ::GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (!(drives & (1u << i))) continue;
    std::wstring path;
    path.push_back(static_cast<wchar_t>(L'A' + i));
    path += L":\\";
    const UINT type = ::GetDriveTypeW(path.c_str());
    if (type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) continue;
    if ((type == DRIVE_REMOVABLE || type == DRIVE_CDROM) && !util::PathExists(path)) continue;
    std::wstring label = path.substr(0, 2);
    wchar_t vol[MAX_PATH] = {};
    if (::GetVolumeInformationW(path.c_str(), vol, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) &&
        vol[0]) {
      label = std::wstring(vol) + L" (" + path.substr(0, 2) + L")";
    }
    auto n = MakeNode(label, path, 0, root.get(), IconId::kFolder);
    n->isRoot = true;
    root->children.push_back(std::move(n));
  }
  wchar_t profile[MAX_PATH] = {};
  if (::SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile) == S_OK) {
    auto n = MakeNode(L"用户目录", profile, 0, root.get(), IconId::kFolder);
    n->isRoot = true;
    root->children.push_back(std::move(n));
  }
  return root;
}

// 选中节点后导航：包内切 innerPath，文件系统切 curDir
void OnNodeSelected(TreeNode* n) {
  if (!n) return;
  if (g_app.archiveMode) {
    if (::CompareStringOrdinal(n->path.c_str(), -1, g_app.innerPath.c_str(), -1, TRUE) !=
        CSTR_EQUAL) {
      g_app.innerPath = n->path;
      AppRefreshView();
      return;
    }
  } else if (!n->path.empty() &&
             ::CompareStringOrdinal(n->path.c_str(), -1, g_app.curDir.c_str(), -1, TRUE) !=
                 CSTR_EQUAL) {
    AppNavigateTo(n->path);
    RefreshToolbarState();
    return;
  }
  RefreshToolbarState();
}

}  // namespace

HWND CreateSideTree(HWND parent, HINSTANCE inst, int id) {
  if (!g_view) {
    g_view = std::make_unique<TreeView>();
    TreeStyle style;
    style.rowHeight = 30;
    style.indent = 18;
    style.padLeft = 14;
    style.iconSize = 16;
    g_view->SetStyle(style);
    g_view->SetChildrenLoader(LoadChildren);
    g_view->SetSelectHandler(OnNodeSelected);
  }
  return g_view->Create(parent, inst, id, L"AIZipSideTree",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL);
}

void RefreshSideTree() {
  if (!g_view || !g_view->hwnd()) return;
  g_view->SetRoot(BuildRoot());
  g_view->SelectPath(g_app.archiveMode ? g_app.innerPath : g_app.curDir);
}

void LayoutSideTree() {
  if (g_view && g_view->hwnd()) g_view->Invalidate();
}

std::wstring TreeActivePath() { return g_view ? g_view->ActivePath() : std::wstring(); }

// 树只列目录，只有包内模式有「解压这个目录」的语义。
// 根节点路径为空，等价于整包。
bool TreeActiveCanExtract() {
  return g_app.archiveMode && g_view && g_view->Active() != nullptr;
}

}  // namespace ui
