// 左侧树模型实现
#include "tree_model.h"

#include "app.h"
#include "util.h"
#include "AiUI/platform.h"

#include <algorithm>

namespace ui {
namespace {

using AiUI::platform;

// 枚举目录下的一级子目录（跳过隐藏与系统目录）
std::vector<std::wstring> subDirs(const std::wstring& dir) {
  std::vector<std::wstring> out;
  // skipHidden 为 true 时平台层已过滤掉隐藏与系统项
  for (const auto& entry : platform()->listDirectory(dir, true)) {
    if (entry.isDir) out.push_back(entry.name);
  }
  std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
    return platform()->compareText(a, b) < 0;
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
      if (!prefix.empty() && !platform()->isPathPrefix(prefix, entry.path)) continue;
      const std::wstring relative = entry.path.substr(prefix.size());
      const size_t slash = relative.find_first_of(L"\\/");
      // 只有还带子路径的条目（或本身是目录）才算树节点
      if (slash == std::wstring::npos && !entry.isDir) continue;
      const std::wstring name = (slash == std::wstring::npos) ? relative : relative.substr(0, slash);
      if (name.empty()) continue;
      if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
      return platform()->compareText(a, b) < 0;
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

  const std::wstring desktop = platform()->specialFolder(AiUI::SpecialFolder::kDesktop);
  if (!desktop.empty()) {
    auto node = makeNode(L"桌面", desktop, 0, root.get(), AiUI::IconId::kHome);
    node->isRoot = true;
    root->children.push_back(std::move(node));
  }

  // 挂载的卷由平台层枚举；空卷（如空读卡器）后端已过滤
  for (const auto& drive : platform()->listDrives()) {
    auto node = makeNode(drive.label, drive.path, 0, root.get(), AiUI::IconId::kFolder);
    node->isRoot = true;
    root->children.push_back(std::move(node));
  }

  const std::wstring home = platform()->specialFolder(AiUI::SpecialFolder::kHome);
  if (!home.empty()) {
    auto node = makeNode(L"用户目录", home, 0, root.get(), AiUI::IconId::kFolder);
    node->isRoot = true;
    root->children.push_back(std::move(node));
  }
  return root;
}

}  // namespace ui
