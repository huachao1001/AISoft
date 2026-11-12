// 主界面 Activity 实现：装配控件、响应交互、窗口级行为
#include "main_activity.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_extract.h"
#include "sevenzip_loader.h"
#include "tree_model.h"
#include "ui_main.h"
#include "util.h"
#include "AiUI/font.h"
#include "AiUI/theme.h"
#include "AiUI/widgets/breadcrumb_bar.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/button_bar.h"
#include "AiUI/widgets/status_bar.h"
#include "AiUI/widgets/tree_view.h"

#include "AiUI/platform.h"

#include <algorithm>

namespace ui {
namespace {

MainActivity* g_activity = nullptr;

// 消息框出口：图标与按钮组合由平台层映射到各系统原生样式
void info(const std::wstring& text, AiUI::NativeHandle owner) {
  AiUI::MessageBoxParams params;
  params.title = L"AIZip";
  params.message = text;
  params.icon = AiUI::MessageBoxIcon::kInfo;
  AiUI::platform()->showMessageBox(owner, params);
}

void warn(const std::wstring& text, AiUI::NativeHandle owner) {
  AiUI::MessageBoxParams params;
  params.title = L"AIZip";
  params.message = text;
  params.icon = AiUI::MessageBoxIcon::kWarning;
  AiUI::platform()->showMessageBox(owner, params);
}

// 返回 true 表示用户选了「是」；默认停在「否」，避免误删
bool confirm(const std::wstring& title, const std::wstring& text, AiUI::NativeHandle owner) {
  AiUI::MessageBoxParams params;
  params.title = title;
  params.message = text;
  params.icon = AiUI::MessageBoxIcon::kWarning;
  params.buttons = AiUI::MessageBoxButtons::kYesNo;
  params.defaultButton = AiUI::MessageBoxResult::kNo;
  return AiUI::platform()->showMessageBox(owner, params) == AiUI::MessageBoxResult::kYes;
}

// 包内路径是否存在：任一条目就是它，或在它下面。
// 7z 条目里的分隔符可能是 '/'，两种都要认。
bool innerPathExists(std::wstring inner) {
  if (inner.empty()) return true;
  std::replace(inner.begin(), inner.end(), L'/', L'\\');

  for (const auto& entry : g_app.archive.Entries()) {
    std::wstring path = entry.path;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    if (AiUI::platform()->equalsPath(path, inner)) return true;
    if (AiUI::platform()->isPathPrefix(inner, path)) return true;
  }
  return false;
}

// 拆 "C:\a\b" 或包内 "a\b" 成逐级路径段
std::vector<AiUI::BreadcrumbSegment> buildSegments() {
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

// 行首图标：文件夹用矢量图标，文件用系统关联图标
AiUI::ListIcon itemIcon(const Item& item) {
  AiUI::ListIcon icon;
  if (item.isDir) {
    icon.glyph = AiUI::IconId::kFolder;
    icon.color = AiUI::kColorTextFaint;
    return icon;
  }
  icon.image = AiUI::fileIcon(item.fullPath.empty() ? item.name : item.fullPath, false);
  icon.glyph = AiUI::IconId::kFile;
  icon.color = AiUI::kColorTextFaint;
  return icon;
}

AiUI::ButtonSpec makeButtonSpec(const wchar_t* label, AiUI::IconId icon) {
  AiUI::ButtonSpec spec;
  spec.label = label;
  spec.icon = icon;
  spec.hasIcon = true;
  spec.placement = AiUI::IconPlacement::kLeft;
  spec.style = AiUI::ButtonStyle::kOutline;
  spec.height = 32;
  spec.iconSize = 18;
  spec.padX = 12;
  spec.iconGap = 6;
  spec.radius = 7;
  return spec;
}

}  // namespace

MainActivity::MainActivity() {
  AiUI::WindowStyle style;
  style.topLevel = true;
  style.resizable = true;
  style.minimizable = true;
  style.maximizable = true;
  style.captionHeight = 0;
  setStyle(style);
  setMinSize(kMinWidth, kMinHeight);
  setTitle(L"AIZip");
  // 主窗口不响应 Esc
  setEscapeCloses(false);
}

MainActivity::~MainActivity() {
  if (g_activity == this) g_activity = nullptr;
}

// ───────────────────────── 装配 ─────────────────────────

void MainActivity::onCreate() {
  g_activity = this;

  crumb_ = findView<AiUI::BreadcrumbBar>("crumb");
  toolBar_ = findView<AiUI::ButtonBar>("toolBar");
  tree_ = findView<AiUI::TreeView>("tree");
  list_ = findView<AiUI::ListView>("list");
  status_ = findView<AiUI::StatusBar>("status");

  attachCrumbBar();
  attachToolBar();
  attachSideTree();
  attachFileList();
  attachStatusBar();

  // 拖放与单实例转发挂在 AiUI 建的顶层窗口上
  g_app.mainWnd = nativeWindow();
  AttachHostHooks();
}

AiUI::NativeHandle MainActivity::nativeWindow() const {
  return window() ? window()->handle() : nullptr;
}

void MainActivity::attachCrumbBar() {
  if (!crumb_) return;

  AiUI::BreadcrumbStyle style;
  style.padX = 16;
  style.nodePadX = 7;
  style.nodePadY = 4;
  style.nodeGap = 2;
  style.searchWidth = 270;
  crumb_->setStyle(style);

  AiUI::BreadcrumbCallbacks callbacks;
  callbacks.onBack = [] {
    if (AppCanBack()) AppGoBack();
  };
  callbacks.onNavigate = [this](const std::wstring& tag) {
    if (g_app.archiveMode) {
      if (tag == g_app.innerPath) return;
      g_app.innerPath = tag;
      AppRefreshView();
      return;
    }
    if (!tag.empty()) AppNavigateTo(tag);
  };
  callbacks.onSearch = [this](const std::wstring& text) { setFilterText(text); };
  callbacks.addressText = [] {
    if (!g_app.archiveMode) return g_app.curDir;
    std::wstring text = g_app.archivePath;
    if (!g_app.innerPath.empty()) text += L"\\" + g_app.innerPath;
    return text;
  };
  callbacks.onCommitAddress = [this](const std::wstring& target) { commitAddress(target); };
  callbacks.onEditClosed = [this] { focusFileList(); };
  crumb_->setCallbacks(std::move(callbacks));

  refreshCrumbBar();
}

void MainActivity::attachToolBar() {
  if (!toolBar_) return;

  btnRefresh_ = toolBar_->addButton(makeButtonSpec(L"刷新", AiUI::IconId::kRefresh), 0);
  if (btnRefresh_) btnRefresh_->setOnClick([] { AppRefreshView(); });

  btnExtractSel_ = toolBar_->addButton(makeButtonSpec(L"提取选中", AiUI::IconId::kExtract), 1);
  if (btnExtractSel_) btnExtractSel_->setOnClick([this] { extractSelection(); });

  btnExtractAll_ = toolBar_->addButton(makeButtonSpec(L"全部解压", AiUI::IconId::kExtractAll), 1);
  if (btnExtractAll_) {
    btnExtractAll_->setOnClick([] { ShowExtractDialog(g_app.mainWnd, {}); });
  }

  btnAdd_ = toolBar_->addButton(makeButtonSpec(L"添加文件", AiUI::IconId::kAdd), 2);
  if (btnAdd_) btnAdd_->setOnClick([this] { ShowCompressDialog(g_app.mainWnd, selectedPaths()); });

  btnDelete_ = toolBar_->addButton(makeButtonSpec(L"删除选中", AiUI::IconId::kTrash), 2);
  if (btnDelete_) btnDelete_->setOnClick([this] { deleteItems(selectedItemIndices()); });

  refreshToolBar();
}

void MainActivity::attachSideTree() {
  if (!tree_) return;

  AiUI::TreeStyle style;
  style.rowHeight = 30;
  style.indent = 18;
  style.padLeft = 14;
  style.iconSize = 16;
  tree_->setStyle(style);

  // loader 与 select 都转给成员函数，树本身不认识应用状态
  tree_->setChildrenLoader([](AiUI::TreeNode* node) { return TreeModel::loadChildren(node); });
  tree_->setSelectHandler([this](AiUI::TreeNode* node) { onTreeNodeSelected(node); });

  rebuildSideTree();
}

void MainActivity::attachFileList() {
  if (!list_) return;

  AiUI::ListStyle style;
  style.headHeight = AiUI::kSizeHeaderH;
  style.rowHeight = AiUI::kSizeRowH;
  style.padX = 16;
  style.iconSize = 18;
  list_->setStyle(style);

  model_.sort(g_app.settings.listSortColumn, g_app.settings.listSortAsc);
  if (model_.sortColumn() >= kColCount) model_.sort(kColName, true);

  std::vector<AiUI::ListColumn> columns;
  static const wchar_t* kTitles[kColCount] = {L"名称", L"类型", L"大小", L"修改时间"};
  static const int kWidths[kColCount] = {320, 110, 100, 160};
  static const bool kRight[kColCount] = {false, false, true, false};
  columns.reserve(kColCount);
  for (int i = 0; i < kColCount; ++i) {
    AiUI::ListColumn column;
    column.title = kTitles[i];
    column.width = kWidths[i];  // 设计值，库内按 DPI 换算
    column.align = kRight[i] ? AiUI::ListAlign::kRight : AiUI::ListAlign::kLeft;
    column.stretch = (i == kColName);  // 名称列吃掉剩余宽度
    columns.push_back(column);
  }
  list_->setColumns(columns);

  // 持久化的是物理像素，恢复时直接给，避免再乘一次 DPI
  for (int i = 0; i < kColCount; ++i) list_->setUserWidth(i, g_app.settings.listColWidth[i]);
  list_->setSort(model_.sortColumn(), model_.sortAscending());

  AiUI::ListCallbacks callbacks;
  callbacks.onActivate = [this](int row) { onRowActivated(row); };
  callbacks.onSelectionChanged = [this] {
    refreshStatusBar();
    refreshToolBar();
  };
  callbacks.onSort = [this](int column, bool ascending) {
    g_app.settings.listSortColumn = column;
    g_app.settings.listSortAsc = ascending;
    model_.sort(column, ascending);
    refreshFileList();
  };
  callbacks.onContextMenu = [this] { showListContextMenu(); };
  callbacks.onKeyDown = [this](AiUI::Key key) { onListKeyDown(key); };
  callbacks.onColumnResized = [this] {
    if (!list_) return;
    for (int i = 0; i < kColCount; ++i) {
      const int width = list_->columnWidth(i);
      if (width > 0) g_app.settings.listColWidth[i] = width;
    }
  };
  list_->setCallbacks(callbacks);

  // 适配器只记数据访问方式，具体某行画什么等滚动到那里再算
  adapter_ = std::make_shared<AiUI::ListRowSource>();
  adapter_->setCount([this] { return model_.rowCount(); });
  adapter_->setBinder([this](int row, AiUI::ListRow& out) {
    const Item* item = model_.itemAt(row);
    if (!item) return;
    // tag 记住条目下标，交互时再翻回来
    out.tag = model_.itemIndexAt(row);
    out.icon = itemIcon(*item);
    if (out.cells.size() < static_cast<size_t>(kColCount)) out.cells.resize(kColCount);
    for (int col = 0; col < kColCount; ++col) {
      out.cells[col].text = FileListModel::cellText(col, *item);
      out.cells[col].textColor = AiUI::kNoColor;
    }
  });
  adapter_->setTagAt([this](int row) { return model_.itemIndexAt(row); });
  list_->setAdapter(adapter_);

  refreshFileList();
}

void MainActivity::attachStatusBar() {
  if (!status_) return;
  AiUI::StatusStyle style;
  style.background = AiUI::kColorTool;
  status_->setStyle(style);
  refreshStatusBar();
}

// ───────────────────────── 刷新 ─────────────────────────

void MainActivity::refreshFileList() {
  model_.sort(model_.sortColumn(), model_.sortAscending());
  model_.rebuild();
  if (!list_) return;
  // 只告诉控件行数变了，不预先生成任何一行
  list_->setSort(model_.sortColumn(), model_.sortAscending());
  list_->notifyDataSetChanged();
}

void MainActivity::refreshStatusBar() {
  if (!status_) return;

  AiUI::StatusFields fields;
  if (g_app.archiveMode) {
    fields.left = g_app.archivePath;
    fields.tag = z7::FormatNameFromExtension(util::FileExtension(g_app.archivePath));
    fields.size = util::FormatSize(AiUI::platform()->fileSize(g_app.archivePath));
    fields.showHome = true;
  } else {
    fields.left = g_app.curDir;
  }

  size_t files = 0;
  size_t dirs = 0;
  uint64_t total = 0;
  for (int index : model_.rows()) {
    if (index < 0 || index >= static_cast<int>(g_app.items.size())) continue;
    const Item& item = g_app.items[index];
    if (item.isDir) {
      ++dirs;
    } else {
      ++files;
      total += item.size;
    }
  }

  if (g_app.archiveMode) {
    fields.right = L"压缩包内：总计 " + std::to_wstring(dirs) + L" 个文件夹，" +
                   std::to_wstring(files) + L" 个文件";
  } else if (files > 0 && dirs > 0) {
    fields.right = L"总计 " + std::to_wstring(files) + L" 个文件、" + std::to_wstring(dirs) +
                   L" 个文件夹";
  } else if (dirs > 0) {
    fields.right = L"总计 " + std::to_wstring(dirs) + L" 个文件夹";
  } else if (files > 0) {
    fields.right = L"总计 " + std::to_wstring(files) + L" 个文件";
  } else {
    fields.right = L"没有项目";
  }
  if (!g_app.archiveMode && files > 0) fields.right += L"（" + util::FormatSize(total) + L"）";

  const int selected = static_cast<int>(selectedItemIndices().size());
  if (selected > 0) fields.right += L"，已选中 " + std::to_wstring(selected) + L" 项";

  status_->setFields(fields);
}

void MainActivity::refreshToolBar() {
  if (!toolBar_) return;
  if (btnExtractSel_) btnExtractSel_->setEnabled(canExtractSelected());
  if (btnDelete_) btnDelete_->setEnabled(!selectedItemIndices().empty());
  if (btnAdd_) btnAdd_->setEnabled(!g_app.archiveMode);
  if (btnRefresh_) btnRefresh_->setEnabled(true);
  if (btnExtractAll_) btnExtractAll_->setEnabled(true);
  toolBar_->relayout();
}

void MainActivity::setFilterText(const std::wstring& text) {
  if (text == model_.filter()) return;
  model_.setFilter(text);
  refreshFileList();
  refreshStatusBar();
}

void MainActivity::updateTitle() {
  const std::wstring title =
      g_app.archiveMode ? util::BaseName(g_app.archivePath) + L" - AIZip" : L"AIZip";
  setTitle(title);
}

void MainActivity::refreshAddressBar() {
  refreshCrumbBar();
  syncSideTree();
}

void MainActivity::refreshCrumbBar() {
  if (!crumb_) return;
  crumb_->setSegments(buildSegments());
  crumb_->setBackEnabled(AppCanBack());
  crumb_->setSearchCue(g_app.archiveMode ? L"在压缩包内搜索文件或文件夹" : L"搜索文件或文件夹");
}

void MainActivity::focusFileList() {
  if (list_) list_->setFocus();
}

// ───────────────────────── 树 ─────────────────────────

void MainActivity::rebuildSideTree() {
  if (!tree_) return;
  rootsKey_ = TreeModel::syncKey();
  tree_->setRoot(TreeModel::buildRoot());
  tree_->selectPath(g_app.archiveMode ? g_app.innerPath : g_app.curDir);
}

void MainActivity::syncSideTree() {
  if (!tree_) return;
  // 根结构只在"文件系统 ↔ 压缩包"或换了包时变，其余情况保留展开状态
  if (TreeModel::syncKey() != rootsKey_) {
    rebuildSideTree();
    return;
  }
  // 只走 selectPath：已展开的分支保持原样，滚动位置也不重置
  tree_->selectPath(g_app.archiveMode ? g_app.innerPath : g_app.curDir);
}

void MainActivity::onTreeNodeSelected(AiUI::TreeNode* node) {
  if (!node) return;
  if (g_app.archiveMode) {
    if (!AiUI::platform()->equalsPath(node->path, g_app.innerPath)) {
      // 包内换目录必须按新前缀重新筛条目，走完整刷新；
      // 树这边 syncSideTree 会按根标识判断，只同步选中、不重建
      g_app.innerPath = node->path;
      AppRefreshView();
      return;
    }
  } else if (!node->path.empty() && !AiUI::platform()->equalsPath(node->path, g_app.curDir)) {
    AppNavigateTo(node->path);
    return;
  }
  // 已经在这个目录上：只更新工具栏，不触发任何重载
  refreshToolBar();
}

std::wstring MainActivity::treeActivePath() const {
  return tree_ ? tree_->activePath() : std::wstring();
}

bool MainActivity::treeActiveCanExtract() const {
  return g_app.archiveMode && tree_ && tree_->active() != nullptr;
}

// ───────────────────────── 列表交互 ─────────────────────────

std::vector<int> MainActivity::selectedItemIndices() const {
  if (!list_) return {};
  return model_.selectedItemIndices(list_->selectedRows());
}

std::vector<std::wstring> MainActivity::selectedPaths() const {
  std::vector<std::wstring> out;
  for (int index : selectedItemIndices()) {
    if (index >= 0 && index < static_cast<int>(g_app.items.size())) {
      out.push_back(g_app.items[index].fullPath);
    }
  }
  if (out.empty()) out.push_back(g_app.curDir);
  return out;
}

void MainActivity::onRowActivated(int row) {
  const Item* item = model_.itemAt(row);
  if (!item) return;
  const AiUI::NativeHandle host = nativeWindow();

  if (g_app.archiveMode) {
    if (item->isDir) {
      g_app.innerPath =
          g_app.innerPath.empty() ? item->name : g_app.innerPath + L"\\" + item->name;
      AppRefreshView();
      return;
    }
    ShowExtractDialog(host, {list_ ? list_->rowTag(row) : -1});
    return;
  }

  if (item->isDir) {
    AppNavigateTo(item->fullPath);
    return;
  }
  if (arc::Archive::IsArchiveFile(item->fullPath)) {
    AppOpenArchive(item->fullPath);
    return;
  }
  AiUI::platform()->openWithSystem(item->fullPath);
}

void MainActivity::showListContextMenu() {
  const auto selected = selectedItemIndices();
  const bool hasSelection = !selected.empty();
  const AiUI::NativeHandle host = nativeWindow();
  const bool canExtract = canExtractSelected();

  // 顺序即下标：showPopupMenu 返回的也是下标
  std::vector<AiUI::MenuItem> menu;
  AiUI::MenuItem extractItem;
  extractItem.label = L"解压到...";
  extractItem.enabled = canExtract;
  menu.push_back(extractItem);

  AiUI::MenuItem compressItem;
  compressItem.label = L"压缩为...";
  menu.push_back(compressItem);

  AiUI::MenuItem sep1;
  sep1.separator = true;
  menu.push_back(sep1);

  AiUI::MenuItem deleteItem;
  deleteItem.label = L"删除";
  deleteItem.enabled = hasSelection;
  menu.push_back(deleteItem);

  AiUI::MenuItem sep2;
  sep2.separator = true;
  menu.push_back(sep2);

  AiUI::MenuItem refreshItem;
  refreshItem.label = L"刷新";
  menu.push_back(refreshItem);

  AiUI::MenuItem propsItem;
  propsItem.label = L"属性";
  propsItem.enabled = hasSelection;
  menu.push_back(propsItem);

  auto* platform = AiUI::platform();
  // 弹出菜单必须先让窗口在前台，否则点别处菜单不消失
  platform->bringToFront(host);
  const int command = platform->showPopupMenu(host, platform->cursorPosition(), menu);

  switch (command) {
    case 0:
      extractSelection();
      break;
    case 1: {
      if (g_app.archiveMode) {
        info(L"压缩包内无法直接创建新压缩包，请先解压。", host);
        break;
      }
      ShowCompressDialog(host, selectedPaths());
      break;
    }
    case 3:
      deleteItems(selected);
      break;
    case 5:
      AppRefreshView();
      break;
    case 7:
      if (hasSelection && !g_app.archiveMode) {
        platform->showProperties(host, g_app.items[selected[0]].fullPath);
      }
      break;
    default:
      break;
  }
}

void MainActivity::onListKeyDown(AiUI::Key key) {
  switch (key) {
    case AiUI::Key::kDelete:
      deleteItems(selectedItemIndices());
      break;
    case AiUI::Key::kBackspace:
      AppGoUp();
      break;
    default:
      break;
  }
}

// ───────────────────────── 文件操作 ─────────────────────────

// 包内模式天然可解压；文件系统下只认单个受支持的压缩包，多选或普通文件一律不可。
// 右侧没有选中项时，退回左侧树当前节点判断。
bool MainActivity::canExtractSelected() const {
  return FileListModel::canExtract(selectedItemIndices(), treeActiveCanExtract());
}

// 解压入口：右侧有选中项就按选中项，否则用左侧树的当前节点
void MainActivity::extractSelection() {
  const auto selected = selectedItemIndices();
  const AiUI::NativeHandle host = nativeWindow();
  if (!selected.empty()) {
    if (g_app.archiveMode) {
      ShowExtractDialog(host, selected);
    } else if (selected.size() == 1) {
      const int index = selected[0];
      if (index >= 0 && index < static_cast<int>(g_app.items.size())) {
        AppExtractArchive(host, g_app.items[index].fullPath);
      }
    }
    return;
  }

  // 右侧没选中：退到左侧树的当前节点
  if (!treeActiveCanExtract()) return;
  const std::wstring path = treeActivePath();
  if (!g_app.archiveMode) {
    AppExtractArchive(host, path);
    return;
  }
  // 包内树节点按目录前缀解压；根节点（空路径）等于整包
  ShowExtractDialog(host, {}, path);
}

void MainActivity::deleteItems(const std::vector<int>& indices) {
  const AiUI::NativeHandle host = nativeWindow();
  if (g_app.archiveMode) {
    info(L"压缩包内的条目不支持直接删除。\n请解压后重新打包。", host);
    return;
  }
  if (indices.empty()) {
    info(L"请先选择要删除的文件或文件夹。", host);
    return;
  }

  const std::wstring message =
      L"确定把选中的 " + std::to_wstring(indices.size()) + L" 项移到回收站吗？";
  if (!confirm(L"确认删除", message, host)) return;

  bool failed = false;
  for (int index : indices) {
    if (index < 0 || index >= static_cast<int>(g_app.items.size())) continue;
    // 移到回收站而非直接删除：误删后还能还原
    if (!AiUI::platform()->moveToTrash(g_app.items[index].fullPath)) failed = true;
  }
  if (failed) warn(L"部分项目删除失败。", host);
  AppRefreshView();
}

// ───────────────────────── 窗口级 ─────────────────────────

void MainActivity::commitAddress(const std::wstring& target) {
  if (target.empty()) return;
  const AiUI::NativeHandle host = nativeWindow();

  if (g_app.archiveMode) {
    std::wstring inner;
    const std::wstring base = g_app.archivePath;
    if (target.size() > base.size() && AiUI::platform()->isPathPrefix(base, target)) {
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
      if (!innerPathExists(inner)) {
        warn(L"压缩包内没有该路径：\n" + inner, host);
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
    warn(L"路径不存在：\n" + target, host);
  }
}

bool MainActivity::onKeyDown(const AiUI::Key& key, const AiUI::Modifiers& mods) {
  if (!mods.ctrl) return false;
  switch (key) {
    case AiUI::Key::kL:
      if (crumb_) crumb_->beginEdit();
      return true;
    case AiUI::Key::kF:
      if (crumb_) crumb_->focusSearch();
      return true;
    default:
      return false;
  }
}

bool MainActivity::onCloseRequest() {
  // 关闭前保存窗口状态；正常尺寸取还原态，最大化时也记下来
  if (AiUI::Window* win = window()) {
    if (AiUI::PlatformWindow* pw = win->platformWindow()) {
      g_app.settings.winMax = pw->isMaximized();
      if (!g_app.settings.winMax) {
        const AiUI::Rect rc = pw->normalBounds();
        g_app.settings.winW = rc.right - rc.left;
        g_app.settings.winH = rc.bottom - rc.top;
      }
    }
  }
  util::SaveSettings(g_app.settings);
  return true;
}

MainActivity* mainActivity() { return g_activity; }

}  // namespace ui
