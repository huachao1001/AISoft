// BreadcrumbBar 实现：节点排布、后退按钮、地址编辑、搜索框
#include "AiUI/widgets/breadcrumb_bar.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/widgets/input_box.h"

#include <algorithm>

namespace AiUI {
namespace {

// 后退按钮与第一个节点之间的间距
constexpr int kBackGap = 6;
// 节点与分隔箭头之间留的呼吸位
constexpr int kChevronWidth = 16;

}  // namespace

BreadcrumbBar::BreadcrumbBar() = default;

BreadcrumbBar::~BreadcrumbBar() {
  // 子视图归基类所有，这里只断开引用
  search_ = nullptr;
  edit_ = nullptr;
}

void BreadcrumbBar::setSegments(std::vector<BreadcrumbSegment> segments) {
  nodes_.clear();
  nodes_.reserve(segments.size());
  for (auto& segment : segments) {
    Node node;
    node.data = std::move(segment);
    nodes_.push_back(std::move(node));
  }
  // 节点变了，命中区必须重算，否则新节点没有可绘制区域
  layoutNodes();
  invalidate();
}

void BreadcrumbBar::setStyle(const BreadcrumbStyle& style) {
  style_ = style;
  layoutNodes();
  invalidate();
}

void BreadcrumbBar::setCallbacks(BreadcrumbCallbacks callbacks) {
  callbacks_ = std::move(callbacks);
  // 回调里带地址文本，尺寸变化后重排一次让输入框拿到新宽度
  if (search_) search_->layout(searchRect());
}

void BreadcrumbBar::setBackEnabled(bool enabled) {
  if (backEnabled_ == enabled) return;
  backEnabled_ = enabled;
  invalidate();
}

void BreadcrumbBar::setSearchCue(const std::wstring& cue) {
  if (search_) search_->setCue(cue);
}

void BreadcrumbBar::clearSearch() {
  if (search_) search_->setText(std::wstring());
}

void BreadcrumbBar::onCreated() {
  search_ = new InputBox();
  InputSpec spec;
  spec.cue = L"搜索文件或文件夹";
  spec.hasLeadIcon = true;
  spec.leadIcon = IconId::kSearch;
  spec.leadIconSize = 14;
  spec.leadIconGap = 5;
  spec.padX = 11;
  spec.radius = 7;
  spec.outerBackground = style_.background;
  search_->setSpec(spec);
  search_->setOnChange([this] {
    if (callbacks_.onSearch) callbacks_.onSearch(search_->text());
  });
  addView(search_);
  layoutNodes();
}

// ───────────────────────── 排布 ─────────────────────────

Rect BreadcrumbBar::backRect() const {
  const Rect rc = clientRect();
  const int w = px(style_.backWidth);
  const int h = px(style_.backHeight);
  const int y = rc.top + (rc.height() - h) / 2;
  const int x = rc.left + px(style_.padX);
  return Rect{x, y, x + w, y + h};
}

// 搜索框贴右，宽度随窗口收缩，给面包屑留出后退按钮 + 一段路径的空间
Rect BreadcrumbBar::searchRect() const {
  const Rect rc = clientRect();
  const int pad = px(style_.padX);
  const int h = px(style_.searchHeight);
  const int minCrumb = px(style_.backWidth) + px(120);
  const int available = rc.width() - pad * 2 - minCrumb;
  const int w = (std::max)(px(120), (std::min)(px(style_.searchWidth), available));
  const int y = rc.top + (rc.height() - h) / 2;
  return Rect{rc.right - pad - w, y, rc.right - pad, y + h};
}

void BreadcrumbBar::layoutNodes() {
  const Rect bar = clientRect();
  int x = backRect().right + px(kBackGap);
  // 面包屑最多占到搜索框左侧，超出的节点不绘制也不可点
  const int limit = searchRect().left - px(8);

  for (size_t i = 0; i < nodes_.size(); ++i) {
    Node& node = nodes_[i];
    int w = platform()->measureText(node.data.label, fonts.ui) + px(style_.nodePadX) * 2;
    if (node.data.home) w += px(style_.iconSize) + px(style_.iconGap);
    const int h = px(style_.nodePadY) * 2 + px(20);
    const int y = bar.top + (bar.height() - h) / 2;
    node.rect = Rect{x, y, x + w, y + h};

    if (node.rect.right > limit) {
      for (size_t j = i; j < nodes_.size(); ++j) nodes_[j].rect = Rect{};
      break;
    }
    x += w;
    if (i + 1 < nodes_.size()) x += px(kChevronWidth) + px(style_.nodeGap) * 2;
  }
}

void BreadcrumbBar::arrangeChildren(const Rect& content) {
  (void)content;
  // 节点与搜索框不按布局参数走，位置由自身宽度算出
  layoutNodes();
  if (search_) search_->layout(searchRect());

  if (editing_ && edit_) {
    const Rect bar = clientRect();
    const int h = px(style_.editHeight);
    const int y = bar.top + (bar.height() - h) / 2;
    // 地址框占住整个路径区，右侧停在搜索框左边
    const int left = bar.left + px(style_.padX);
    const int right = searchRect().left - px(8);
    edit_->layout(Rect{left, y, right, y + h});
  }
}

int BreadcrumbBar::hitNode(Point pt) const {
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (!nodes_[i].rect.isEmpty() && nodes_[i].rect.contains(pt)) return static_cast<int>(i);
  }
  return -1;
}

// ───────────────────────── 绘制 ─────────────────────────

void BreadcrumbBar::onPaint(Painter& painter, const Rect& rect) {
  painter.fillRect(rect, style_.background);

  if (editing_ && edit_) {
    painter.drawHLine(rect.left, rect.right, rect.bottom - 1, style_.line);
    return;
  }

  const Rect back = backRect();
  if (backHot_) painter.fillRoundRect(back, kColorHover, px(6));

  const int bs = px(18);
  painter.drawIcon(IconId::kArrowBack,
                   Rect{back.left + (back.width() - bs) / 2, back.top + (back.height() - bs) / 2,
                        back.left + (back.width() + bs) / 2,
                        back.top + (back.height() + bs) / 2},
                   backEnabled_ ? style_.backInk : style_.backInkDisabled);

  for (size_t i = 0; i < nodes_.size(); ++i) {
    const Node& node = nodes_[i];
    if (node.rect.isEmpty() || node.rect.right > rect.right) break;

    const Color ink = node.data.current ? style_.textCurrent : style_.text;
    if (node.data.current) {
      painter.fillRoundRect(node.rect, style_.currentBg, px(style_.radius));
    } else if (hot_ == static_cast<int>(i)) {
      painter.fillRoundRect(node.rect, style_.hoverBg, px(style_.radius));
    }

    int textLeft = node.rect.left + px(style_.nodePadX);
    if (node.data.home) {
      const int is = px(style_.iconSize);
      const int y = node.rect.top + (node.rect.height() - is) / 2;
      painter.drawIcon(IconId::kHome, Rect{textLeft, y, textLeft + is, y + is},
                       node.data.current ? kColorInk : style_.text);
      textLeft += is + px(style_.iconGap);
    }
    painter.drawText(
        Rect{textLeft, node.rect.top, node.rect.right - px(style_.nodePadX), node.rect.bottom},
        node.data.label, ink, fonts.ui);

    // 分隔箭头
    if (i + 1 < nodes_.size()) {
      const int sepW = px(kChevronWidth) + px(style_.nodeGap) * 2;
      const Rect sep{node.rect.right + px(style_.nodeGap), node.rect.top,
                     node.rect.right + sepW, node.rect.bottom};
      if (sep.right > rect.right) break;
      const int cs = px(14);
      painter.drawIcon(IconId::kChevron,
                       Rect{sep.left + (sep.width() - cs) / 2,
                            sep.top + (sep.height() - cs) / 2,
                            sep.left + (sep.width() + cs) / 2,
                            sep.top + (sep.height() + cs) / 2},
                       style_.separator);
    }
  }

  painter.drawHLine(rect.left, rect.right, rect.bottom - 1, style_.line);
}

// ───────────────────────── 交互 ─────────────────────────

void BreadcrumbBar::onMouseMove(const MouseEvent& e) {
  const int hit = hitNode(e.position);
  const bool backHot = backRect().contains(e.position);
  if (hit != hot_ || backHot != backHot_) {
    hot_ = hit;
    backHot_ = backHot;
    invalidate();
  }
  setCursor(hit >= 0 || backHot ? CursorShape::kHand : CursorShape::kArrow);
}

void BreadcrumbBar::onMouseLeave() {
  if (hot_ != -1 || backHot_) {
    hot_ = -1;
    backHot_ = false;
    invalidate();
  }
}

bool BreadcrumbBar::onMouseDown(const MouseEvent& e) {
  if (e.button != MouseButton::kLeft) return false;
  // 只认落在自身范围内的按下，丢掉切窗残留的消息
  pressed_ = clientRect().contains(e.position);
  return pressed_;
}

void BreadcrumbBar::onMouseUp(const MouseEvent& e) {
  if (!pressed_) return;
  pressed_ = false;

  if (backRect().contains(e.position)) {
    if (backEnabled_ && callbacks_.onBack) callbacks_.onBack();
    return;
  }
  const int hit = hitNode(e.position);
  if (hit >= 0) {
    const Node& node = nodes_[static_cast<size_t>(hit)];
    if (!node.data.current && callbacks_.onNavigate) callbacks_.onNavigate(node.data.tag);
    return;
  }
  // 点空白处：进入编辑
  beginEdit();
}

// ───────────────────────── 地址编辑 ─────────────────────────

void BreadcrumbBar::beginEdit() {
  if (editing_) {
    if (edit_) edit_->setFocus();
    return;
  }
  showEdit();
}

void BreadcrumbBar::focusSearch() {
  if (editing_) hideEdit();
  if (search_) search_->setFocus();
}

void BreadcrumbBar::showEdit() {
  if (!edit_) {
    edit_ = new InputBox();
    InputSpec spec;
    spec.padX = 10;
    spec.radius = 7;  // 圆角由输入框自绘
    spec.hasLeadIcon = false;
    spec.outerBackground = style_.background;
    edit_->setSpec(spec);
    edit_->setOnSubmit([this](bool committed) {
      const std::wstring text = committed ? edit_->text() : std::wstring();
      hideEdit();
      if (committed && callbacks_.onCommitAddress) callbacks_.onCommitAddress(text);
      if (callbacks_.onEditClosed) callbacks_.onEditClosed();
    });
    // 点到别处即放弃编辑
    edit_->setOnBlur([this] {
      if (!editing_) return;
      hideEdit();
      if (callbacks_.onEditClosed) callbacks_.onEditClosed();
    });
    addView(edit_);
  }

  const std::wstring text = callbacks_.addressText ? callbacks_.addressText() : std::wstring();
  edit_->setText(text);
  edit_->selectAll();
  edit_->show(true);

  editing_ = true;
  arrangeChildren(contentRect());
  edit_->setFocus();
  invalidate();
}

void BreadcrumbBar::hideEdit() {
  if (edit_) edit_->show(false);
  editing_ = false;
  invalidate();
}

}  // namespace AiUI
