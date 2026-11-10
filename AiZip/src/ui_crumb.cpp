// 面包屑导航条：后退按钮 + 路径节点 + 搜索框。
// 平时只读展示，点空白处或 Ctrl+L 切成可编辑输入框。
#include "ui_crumb.h"

#include "app.h"
#include "ui_list.h"
#include "ui_main.h"
#include "util.h"
#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/theme.h"
#include "AiUI/widgets/input_box.h"

#include <algorithm>

namespace ui {
namespace {

// 节点样式：padding 4px 7px，圆角 5
constexpr int kNodePadX = 7;
constexpr int kNodePadY = 4;
constexpr int kNodeGap = 2;
constexpr int kBackBtnW = 30;
constexpr int kBackBtnH = 30;
constexpr int kIconSm = 16;
constexpr int kIconGap = 6;  // 图标与文字的间距，宽度计算与绘制必须一致
constexpr int kSearchW = 270;

}  // namespace

CrumbBar::CrumbBar() { className_ = L"AIZIPCrumbBar"; }

CrumbBar::~CrumbBar() {
  // 子控件归基类所有，这里只断开引用
  search_ = nullptr;
  edit_ = nullptr;
}

void CrumbBar::onCreated() {
  search_ = new AiUI::InputBox();
  AiUI::InputSpec spec;
  spec.cue = L"搜索文件或文件夹";
  spec.hasLeadIcon = true;
  spec.leadIcon = AiUI::IconId::kSearch;
  spec.leadIconSize = 14;
  spec.leadIconGap = 5;
  spec.padX = 11;
  spec.radius = 7;
  search_->setSpec(spec);
  search_->setOnChange([this] { SetFilterText(search_->text()); });
  addChild(search_, AiUI::LayoutParams{});

  buildNodes();
}

void CrumbBar::refresh() {
  buildNodes();
  if (search_) {
    search_->setCue(g_app.archiveMode ? L"在压缩包内搜索文件或文件夹" : L"搜索文件或文件夹");
  }
  // 节点集合变了，矩形也要重算，否则新节点没有可绘制区域
  const AiUI::Rect rc = clientRect();
  arrangeChildren(rc);
  invalidate();
}

// ───────────────────────── 路径节点 ─────────────────────────

void CrumbBar::buildNodes() {
  nodes_.clear();
  if (g_app.archiveMode) {
    Node root;
    root.label = util::BaseName(g_app.archivePath);
    root.target.clear();  // 空 = 包根
    root.current = g_app.innerPath.empty();
    root.home = true;
    nodes_.push_back(root);

    if (!g_app.innerPath.empty()) {
      std::wstring acc;
      size_t pos = 0;
      const std::wstring& path = g_app.innerPath;
      while (pos <= path.size()) {
        size_t sep = path.find(L'\\', pos);
        if (sep == std::wstring::npos) sep = path.size();
        const std::wstring segment = path.substr(pos, sep - pos);
        pos = sep + 1;
        if (segment.empty()) continue;
        if (!acc.empty()) acc += L"\\";
        acc += segment;
        Node node;
        node.label = segment;
        node.target = acc;
        node.current = (sep >= path.size());
        nodes_.push_back(node);
        if (pos > path.size()) break;
      }
    }
    return;
  }

  // 文件系统：拆成 "盘符\a\b\c"
  const std::wstring& dir = g_app.curDir;
  if (dir.empty()) return;
  size_t pos = 0;
  // 盘符根 "C:\"
  if (dir.size() >= 2 && dir[1] == L':') {
    Node node;
    node.label = dir.substr(0, 2);
    node.target = dir.substr(0, 2) + L"\\";
    node.home = true;
    nodes_.push_back(node);
    pos = (dir.size() >= 3 && (dir[2] == L'\\' || dir[2] == L'/')) ? 3 : 2;
  }
  std::wstring acc = nodes_.empty() ? std::wstring() : nodes_[0].target;
  while (pos < dir.size()) {
    size_t sep = dir.find_first_of(L"\\/", pos);
    if (sep == std::wstring::npos) sep = dir.size();
    const std::wstring segment = dir.substr(pos, sep - pos);
    pos = sep + 1;
    if (segment.empty()) continue;
    if (!acc.empty() && acc.back() != L'\\') acc += L'\\';
    acc += segment;
    Node node;
    node.label = segment;
    node.target = acc;
    nodes_.push_back(node);
    if (pos > dir.size()) break;
  }
  if (!nodes_.empty()) nodes_.back().current = true;
}

// 搜索框贴右，宽度随窗口收缩，给面包屑留出后退按钮 + 一段路径的空间
AiUI::Rect CrumbBar::searchRect() const {
  const AiUI::Rect rc = clientRect();
  const int h = px(34);
  const int pad = px(16);
  const int minCrumb = px(kBackBtnW) + px(120);
  const int avail = rc.width() - pad * 2 - minCrumb;
  const int w = (std::max)(px(120), (std::min)(px(kSearchW), avail));
  const int y = (rc.height() - h) / 2;
  return AiUI::Rect{rc.right - pad - w, y, rc.right - pad, y + h};
}

AiUI::Rect CrumbBar::backRect() const {
  const AiUI::Rect rc = clientRect();
  const int y = (rc.height() - px(kBackBtnH)) / 2;
  return AiUI::Rect{px(16), y, px(16) + px(kBackBtnW), y + px(kBackBtnH)};
}

void CrumbBar::layoutNodes() {
  const AiUI::Rect bar = clientRect();
  const AiUI::Rect back = backRect();
  int x = back.right + px(6);
  // 面包屑最多占到搜索框左侧，超出的节点不绘制也不可点
  const int limit = searchRect().left - px(8);

  for (size_t i = 0; i < nodes_.size(); ++i) {
    Node& node = nodes_[i];
    int w = AiUI::platform()->measureText(node.label, AiUI::fonts.ui) + px(kNodePadX) * 2;
    if (node.home) w += px(kIconSm) + px(kIconGap);
    const int h = px(kNodePadY) * 2 + px(20);
    const int y = (bar.height() - h) / 2;
    node.rect = AiUI::Rect{x, y, x + w, y + h};

    if (node.rect.right > limit) {
      // 放不下的节点标记成空矩形，既不绘制也不可点
      for (size_t j = i; j < nodes_.size(); ++j) nodes_[j].rect = AiUI::Rect{};
      break;
    }
    x += w;
    if (i + 1 < nodes_.size()) x += px(16) + px(kNodeGap) * 2;
  }
}

void CrumbBar::arrangeChildren(const AiUI::Rect& content) {
  (void)content;
  layoutNodes();

  const AiUI::Rect search = searchRect();
  if (search_) search_->layout(search);

  if (editing_ && edit_) {
    const int h = px(32);
    const int y = (clientRect().height() - h) / 2;
    const int left = px(16);
    const int right = search.left - px(8);
    edit_->layout(AiUI::Rect{left, y, right, y + h});
  }
}

int CrumbBar::hitNode(AiUI::Point pt) const {
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (!nodes_[i].rect.isEmpty() && nodes_[i].rect.contains(pt)) return static_cast<int>(i);
  }
  return -1;
}

// ───────────────────────── 绘制 ─────────────────────────

void CrumbBar::onPaint(AiUI::Painter& painter, const AiUI::Rect& rect) {
  painter.fillRect(rect, AiUI::kColorPane);

  if (editing_ && edit_) {
    painter.drawHLine(rect.left, rect.right, rect.bottom - 1, AiUI::kColorLine);
    return;
  }

  const AiUI::Rect back = backRect();
  if (backHot_) painter.fillRoundRect(back, AiUI::kColorHover, px(6));

  const bool canBack = AppCanBack();
  const AiUI::Color backInk = canBack ? AiUI::kColorInk : AiUI::kColorTextFaint;
  const int bs = px(18);
  painter.drawIcon(AiUI::IconId::kArrowBack,
                   AiUI::Rect{back.left + (back.width() - bs) / 2,
                              back.top + (back.height() - bs) / 2,
                              back.left + (back.width() + bs) / 2,
                              back.top + (back.height() + bs) / 2},
                   backInk);

  for (size_t i = 0; i < nodes_.size(); ++i) {
    const Node& node = nodes_[i];
    if (node.rect.isEmpty() || node.rect.right > rect.right) break;

    if (node.current) {
      painter.fillRoundRect(node.rect, AiUI::kColorCurrentBg, px(5));
    } else if (hot_ == static_cast<int>(i)) {
      painter.fillRoundRect(node.rect, AiUI::kColorLinkHover, px(5));
    }

    int textLeft = node.rect.left + px(kNodePadX);
    if (node.home) {
      const int is = px(kIconSm);
      const int y = node.rect.top + (node.rect.height() - is) / 2;
      painter.drawIcon(AiUI::IconId::kHome, AiUI::Rect{textLeft, y, textLeft + is, y + is},
                       node.current ? AiUI::kColorInk : AiUI::kColorLink);
      textLeft += is + px(kIconGap);
    }
    painter.drawText(
        AiUI::Rect{textLeft, node.rect.top, node.rect.right - px(kNodePadX), node.rect.bottom},
        node.label, node.current ? AiUI::makeColor(0x11, 0x11, 0x11) : AiUI::kColorLink,
        AiUI::fonts.ui);

    // 分隔箭头
    if (i + 1 < nodes_.size()) {
      const int sepW = px(16) + px(kNodeGap) * 2;
      const AiUI::Rect sep{node.rect.right + px(kNodeGap), node.rect.top,
                           node.rect.right + sepW, node.rect.bottom};
      if (sep.right > rect.right) break;
      const int cs = px(14);
      painter.drawIcon(AiUI::IconId::kChevron,
                       AiUI::Rect{sep.left + (sep.width() - cs) / 2,
                                  sep.top + (sep.height() - cs) / 2,
                                  sep.left + (sep.width() + cs) / 2,
                                  sep.top + (sep.height() + cs) / 2},
                       AiUI::makeColor(0x9c, 0xa3, 0xaf));
    }
  }

  painter.drawHLine(rect.left, rect.right, rect.bottom - 1, AiUI::kColorLine);
}

// ───────────────────────── 交互 ─────────────────────────

void CrumbBar::onMouseMove(const AiUI::MouseEvent& e) {
  const int hit = hitNode(e.position);
  const bool backHot = backRect().contains(e.position);
  if (hit != hot_ || backHot != backHot_) {
    hot_ = hit;
    backHot_ = backHot;
    invalidate();
  }
  setCursor(hit >= 0 || backHot ? AiUI::CursorShape::kHand : AiUI::CursorShape::kArrow);
}

void CrumbBar::onMouseLeave() {
  if (hot_ != -1 || backHot_) {
    hot_ = -1;
    backHot_ = false;
    invalidate();
  }
}

void CrumbBar::onMouseDown(const AiUI::MouseEvent& e) {
  // 只有按下发生在自身范围内才算有效点击，丢掉切窗残留的抬起消息
  pressed_ = clientRect().contains(e.position);
}

void CrumbBar::onMouseUp(const AiUI::MouseEvent& e) {
  if (!pressed_) return;
  pressed_ = false;

  if (backRect().contains(e.position)) {
    if (AppCanBack()) AppGoBack();
    return;
  }
  const int hit = hitNode(e.position);
  if (hit >= 0) {
    navTo(nodes_[static_cast<size_t>(hit)]);
    return;
  }
  // 点空白处：进入编辑
  beginEdit();
}

void CrumbBar::navTo(const Node& node) {
  if (g_app.archiveMode) {
    if (node.target == g_app.innerPath) return;
    g_app.innerPath = node.target;
    AppRefreshView();
  } else if (!node.target.empty()) {
    AppNavigateTo(node.target);
  }
}

// ───────────────────────── 地址输入 ─────────────────────────

void CrumbBar::beginEdit() {
  if (editing_) {
    if (edit_) edit_->setFocus();
    return;
  }
  showEdit();
}

void CrumbBar::focusSearch() {
  if (editing_) hideEdit();
  if (search_) search_->setFocus();
}
void CrumbBar::showEdit() {
  if (!edit_) {
    edit_ = new AiUI::InputBox();
    AiUI::InputSpec spec;
    spec.padX = 10;
    spec.radius = 7;  // 圆角由输入框自绘
    spec.hasLeadIcon = false;
    edit_->setSpec(spec);
    edit_->setOnSubmit([this](bool committed) {
      if (committed) {
        commitEdit();
      } else {
        hideEdit();
      }
      FocusFileList();
    });
    // 点到别处即放弃编辑
    edit_->setOnBlur([this] {
      if (editing_) hideEdit();
    });
    addChild(edit_, AiUI::LayoutParams{});
  }

  std::wstring text;
  if (g_app.archiveMode) {
    text = g_app.archivePath;
    if (!g_app.innerPath.empty()) text += L"\\" + g_app.innerPath;
  } else {
    text = g_app.curDir;
  }
  edit_->setText(text);
  edit_->selectAll();
  edit_->show(true);

  editing_ = true;
  repositionEdit();
  edit_->setFocus();
  invalidate();
}

void CrumbBar::hideEdit() {
  if (edit_) edit_->show(false);
  editing_ = false;
  invalidate();
}

void CrumbBar::repositionEdit() {
  if (!editing_) return;
  arrangeChildren(AiUI::Rect{0, 0, clientRect().width(), clientRect().height()});
}

// 包内路径是否存在：任一条目就是它，或在它下面。
// 7z 条目里的分隔符可能是 '/'，两种都要认。
namespace {

bool innerPathExists(std::wstring inner) {
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

}  // namespace

void CrumbBar::commitEdit() {
  if (!edit_) return;
  const std::wstring target = edit_->text();
  hideEdit();
  if (target.empty()) return;

  HWND owner = static_cast<HWND>(handle());

  // 压缩包模式：完整 "包路径\内部\路径"，或直接输入包内相对路径
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
      if (!innerPathExists(inner)) {
        ::MessageBoxW(owner, (L"压缩包内没有该路径：\n" + inner).c_str(), L"AIZip",
                      MB_OK | MB_ICONWARNING);
        invalidate();
        return;
      }
      g_app.innerPath = inner;
      AppRefreshView();
      return;
    }
    // 输入的是别的包或目录
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
    invalidate();
  }
}

}  // namespace ui
