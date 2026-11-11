// 左侧树模型实现
#include "tree_model.h"

#include "app.h"
#include "util.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>

namespace ui {
namespace {

// 枚举目录下的一级子目录（跳过隐藏与系统目录）
std::vector<std::wstring> subDirs(const std::wstring& dir) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW find{};
  HANDLE handle = ::FindFirstFileW(util::JoinPath(dir, L"*").c_str(), &find);
  if (handle == INVALID_HANDLE_VALUE) return out;
  do {
    if (find.cFileName[0] == L'.') continue;
    if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (find.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
    if (find.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;
    out.emplace_back(find.cFileName);
  } while (::FindNextFileW(handle, &find));
  ::FindClose(handle);
  std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
    return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  return out;
}

std::unique_ptr<AiUI::TreeNode> makeNode(const std::wstring& label, const std::wstring& path,
                                         int depth, AiUI::TreeNode* parent, AiUI::IconId icon) {
  auto node = std::make_unique<AiUI::TreeNode>();
  node->label = label;
  node->path = path;
  node->depth = depth;
  node->parent = parent;
  node->icon = icon;
  return node;
}

}  // namespace

std::wstring TreeModel::syncKey() {
  return g_app.archiveMode ? g_app.archivePath : std::wstring(L"fs");
}

std::vector<std::unique_ptr<AiUI::TreeNode>> TreeModel::loadChildren(AiUI::TreeNode* node) {
  std::vector<std::unique_ptr<AiUI::TreeNode>> out;
  if (!node) return out;

  if (g_app.archiveMode) {
    const std::wstring prefix = node->path.empty() ? L"" : node->path + L"\\";
    std::vector<std::wstring> names;
    for (const auto& entry : g_app.archive.Entries()) {
      if (entry.path.size() <= prefix.size()) continue;
      if (!prefix.empty() &&
          ::CompareStringOrdinal(entry.path.c_str(), static_cast<int>(prefix.size()),
                                 prefix.c_str(), static_cast<int>(prefix.size()), TRUE) !=
              CSTR_EQUAL) {
        continue;
      }
      const std::wstring relative = entry.path.substr(prefix.size());
      const size_t slash = relative.find_first_of(L"\\/");
      // 只有还带子路径的条目（或本身是目录）才算树节点
      if (slash == std::wstring::npos && !entry.isDir) continue;
      const std::wstring name = (slash == std::wstring::npos) ? relative : relative.substr(0, slash);
      if (name.empty()) continue;
      if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
      return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    for (const auto& name : names) {
      const std::wstring inner = node->path.empty() ? name : node->path + L"\\" + name;
      out.push_back(makeNode(name, inner, node->depth + 1, node, AiUI::IconId::kFolder));
    }
    return out;
  }

  for (const auto& dir : subDirs(node->path)) {
    out.push_back(
        makeNode(dir, util::JoinPath(node->path, dir), node->depth + 1, node, AiUI::IconId::kFolder));
  }
  return out;
}

std::unique_ptr<AiUI::TreeNode> TreeModel::buildRoot() {
  auto root = std::make_unique<AiUI::TreeNode>();
  root->depth = -1;
  root->loaded = true;
  root->expanded = true;

  if (g_app.archiveMode) {
    auto node = makeNode(util::BaseName(g_app.archivePath) + L"（根目录）", L"", 0, root.get(),
                         AiUI::IconId::kHome);
    node->isRoot = true;
    root->children.push_back(std::move(node));
    return root;
  }

  wchar_t desktop[MAX_PATH] = {};
  if (::SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop) == S_OK) {
    auto node = makeNode(L"桌面", desktop, 0, root.get(), AiUI::IconId::kHome);
    node->isRoot = true;
    root->children.push_back(std::move(node));
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
    wchar_t volume[MAX_PATH] = {};
    if (::GetVolumeInformationW(path.c_str(), volume, MAX_PATH, nullptr, nullptr, nullptr, nullptr,
                                0) &&
        volume[0]) {
      label = std::wstring(volume) + L" (" + path.substr(0, 2) + L")";
    }
    auto node = makeNode(label, path, 0, root.get(), AiUI::IconId::kFolder);
    node->isRoot = true;
    root->children.push_back(std::move(node));
  }
  wchar_t profile[MAX_PATH] = {};
  if (::SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile) == S_OK) {
    auto node = makeNode(L"用户目录", profile, 0, root.get(), AiUI::IconId::kFolder);
    node->isRoot = true;
    root->children.push_back(std::move(node));
  }
  return root;
}

}  // namespace ui
