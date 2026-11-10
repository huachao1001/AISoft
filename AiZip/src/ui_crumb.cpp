// 面包屑适配：把应用状态翻译成 BreadcrumbBar 的路径段与回调
#include "ui_crumb.h"

#include "app.h"
#include "ui_list.h"
#include "ui_main.h"
#include "util.h"
#include "AiUI/widgets/breadcrumb_bar.h"

#include <algorithm>

namespace ui {
namespace {

AiUI::BreadcrumbBar* g_bar = nullptr;

// 包内路径是否存在：任一条目就是它，或在它下面。
// 7z 条目里的分隔符可能是 '/'，两种都要认。
bool InnerPathExists(std::wstring inner) {
  if (inner.empty()) return true;
  std::replace(inner.begin(), inner.end(), L'/', L'\\');

  for (const auto& entry : g_app.archive.Entries()) {
    std::wstring path = entry.path;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    if (::CompareStringOrdinal(path.c_str(), -1, inner.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
    if (path.size() > inner.size() && path[inner.size()] == L'\\' &&
        ::CompareStringOrdinal(path.c_str(), static_cast<int>(inner.size()), inner.c_str(),
                               static_cast<int>(inner.size()), TRUE) == CSTR_EQUAL) {
      return true;
    }
  }
  return false;
}

// 拆 "C:\a\b" 或包内 "a\b" 成逐级路径段
std::vector<AiUI::BreadcrumbSegment> BuildSegments() {
  std::vector<AiUI::BreadcrumbSegment> out;

  if (g_app.archiveMode) {
    AiUI::BreadcrumbSegment root;
    root.label = util::BaseName(g_app.archivePath);
    root.tag.clear();  // 空 = 包根
    root.current = g_app.innerPath.empty();
    root.home = true;
    out.push_back(root);

    std::wstring acc;
    const std::wstring& path = g_app.innerPath;
    size_t pos = 0;
    while (pos < path.size()) {
      size_t sep = path.find(L'\\', pos);
      if (sep == std::wstring::npos) sep = path.size();
      const std::wstring segment = path.substr(pos, sep - pos);
      pos = sep + 1;
      if (segment.empty()) continue;
      if (!acc.empty()) acc += L'\\';
      acc += segment;
      AiUI::BreadcrumbSegment node;
      node.label = segment;
      node.tag = acc;
      node.current = (pos >= path.size());
      out.push_back(node);
    }
    return out;
  }

  const std::wstring& dir = g_app.curDir;
  if (dir.empty()) return out;

  size_t pos = 0;
  // 盘符根 "C:\"
  if (dir.size() >= 2 && dir[1] == L':') {
    AiUI::BreadcrumbSegment node;
    node.label = dir.substr(0, 2);
    node.tag = dir.substr(0, 2) + L"\\";
    node.home = true;
    out.push_back(node);
    pos = (dir.size() >= 3 && (dir[2] == L'\\' || dir[2] == L'/')) ? 3 : 2;
  }
  std::wstring acc = out.empty() ? std::wstring() : out[0].tag;
  while (pos < dir.size()) {
    size_t sep = dir.find_first_of(L"\\/", pos);
    if (sep == std::wstring::npos) sep = dir.size();
    const std::wstring segment = dir.substr(pos, sep - pos);
    pos = sep + 1;
    if (segment.empty()) continue;
    if (!acc.empty() && acc.back() != L'\\') acc += L'\\';
    acc += segment;
    AiUI::BreadcrumbSegment node;
    node.label = segment;
    node.tag = acc;
    out.push_back(node);
  }
  if (!out.empty()) out.back().current = true;
  return out;
}

void OnNavigate(const std::wstring& tag) {
  if (g_app.archiveMode) {
    if (tag == g_app.innerPath) return;
    g_app.innerPath = tag;
    AppRefreshView();
    return;
  }
  if (!tag.empty()) AppNavigateTo(tag);
}

std::wstring CurrentAddress() {
  if (!g_app.archiveMode) return g_app.curDir;
  std::wstring text = g_app.archivePath;
  if (!g_app.innerPath.empty()) text += L"\\" + g_app.innerPath;
  return text;
}

void OnCommitAddress(const std::wstring& target) {
  if (target.empty()) return;
  HWND owner = g_app.mainWnd;

  if (g_app.archiveMode) {
    std::wstring inner;
    const std::wstring base = g_app.archivePath;
    if (target.size() > base.size() &&
        ::CompareStringOrdinal(target.c_str(), static_cast<int>(base.size()), base.c_str(),
                               static_cast<int>(base.size()), TRUE) == CSTR_EQUAL) {
      inner = target.substr(base.size());
      while (!inner.empty() && (inner.front() == L'\\' || inner.front() == L'/')) {
        inner.erase(inner.begin());
      }
    } else if (!util::PathExists(target)) {
      // 不是磁盘上的路径，按包内相对路径理解
      inner = target;
      while (!inner.empty() && (inner.front() == L'\\' || inner.front() == L'/')) {
        inner.erase(inner.begin());
      }
      while (!inner.empty() && (inner.back() == L'\\' || inner.back() == L'/')) inner.pop_back();
      std::replace(inner.begin(), inner.end(), L'/', L'\\');
    }

    if (target == base || !inner.empty()) {
      if (!InnerPathExists(inner)) {
        ::MessageBoxW(owner, (L"压缩包内没有该路径：\n" + inner).c_str(), L"AIZip",
                      MB_OK | MB_ICONWARNING);
        return;
      }
      g_app.innerPath = inner;
      AppRefreshView();
      return;
    }
    // 输入的是别的包
    if (arc::Archive::IsArchiveFile(target) && util::PathExists(target)) {
      AppOpenArchive(target);
      return;
    }
  }

  if (util::IsDirectory(target)) {
    AppNavigateTo(target);
  } else if (arc::Archive::IsArchiveFile(target) && util::PathExists(target)) {
    AppOpenArchive(target);
  } else {
    ::MessageBoxW(owner, (L"路径不存在：\n" + target).c_str(), L"AIZip", MB_OK | MB_ICONWARNING);
  }
}

}  // namespace

void AttachCrumbBar(AiUI::BreadcrumbBar* bar) {
  if (!bar || g_bar) return;
  g_bar = bar;

  AiUI::BreadcrumbStyle style;
  style.padX = 16;
  style.nodePadX = 7;
  style.nodePadY = 4;
  style.nodeGap = 2;
  style.searchWidth = 270;
  bar->setStyle(style);

  AiUI::BreadcrumbCallbacks callbacks;
  callbacks.onBack = [] {
    if (AppCanBack()) AppGoBack();
  };
  callbacks.onNavigate = OnNavigate;
  callbacks.onSearch = SetFilterText;
  callbacks.addressText = CurrentAddress;
  callbacks.onCommitAddress = OnCommitAddress;
  callbacks.onEditClosed = FocusFileList;
  bar->setCallbacks(std::move(callbacks));

  RefreshCrumbBar();
}

void RefreshCrumbBar() {
  if (!g_bar) return;
  g_bar->setSegments(BuildSegments());
  g_bar->setBackEnabled(AppCanBack());
  g_bar->setSearchCue(g_app.archiveMode ? L"在压缩包内搜索文件或文件夹" : L"搜索文件或文件夹");
}

void BeginAddressEdit() {
  if (g_bar) g_bar->beginEdit();
}

void FocusSearchBox() {
  if (g_bar) g_bar->focusSearch();
}

bool IsAddressEditing() { return g_bar && g_bar->isEditing(); }

}  // namespace ui
