// ThumbnailStrip 实现
#include "AiUI/widgets/thumbnail_strip.h"

#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/theme.h"

#include <algorithm>

namespace AiUI {
namespace {

// 缓存上限：滚动浏览一个大目录时总要解一些，但不能无限涨。
// 超过后只保留当前可见的那些，再看回来就重解一次。
constexpr size_t kMaxCached = 160;

// 只取文件名部分：缩略图条宽度有限，目录前缀永远不会显示出来
std::wstring fileNameOf(const std::wstring& path) {
  const size_t pos = path.find_last_of(L"\\/");
  return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

}  // namespace

ThumbnailStrip::ThumbnailStrip() {
  // 缩略图条不需要横向滚动：宽度由布局定死，内容永远按宽度排
  setHorizontalScrollEnabled(false);
}

ThumbnailStrip::~ThumbnailStrip() { releaseThumbnails(); }

// ───────────────────────── 数据 ─────────────────────────

void ThumbnailStrip::setItems(std::vector<std::wstring> paths) {
  if (paths == items_) return;
  releaseThumbnails();
  items_ = std::move(paths);
  if (selected_ >= itemCount()) selected_ = itemCount() - 1;
  setScrollOffset(0);
  requestLayout();
  invalidate();
}

void ThumbnailStrip::setSelected(int index) {
  const int next = (index >= 0 && index < itemCount()) ? index : -1;
  if (next == selected_) return;
  selected_ = next;
  invalidate();
}

void ThumbnailStrip::setStyle(const ThumbnailStyle& style) {
  style_ = style;
  requestLayout();
  invalidate();
}

void ThumbnailStrip::scrollToItem(int index) {
  if (index < 0 || index >= itemCount()) return;
  // 还没布局时视口高度是 0，任何一项都"看不见"，照公式算会把条目凭空滚出去一行
  const int viewport = viewportHeight();
  if (viewport <= 0) return;

  const Rect rc = itemRect(index);
  if (rc.top < 0) {
    setScrollOffset(scrollOffset() + rc.top);
  } else if (rc.bottom > viewport) {
    setScrollOffset(scrollOffset() + rc.bottom - viewport);
  }
}

// ───────────────────────── 几何 ─────────────────────────

int ThumbnailStrip::itemHeight() const {
  const int d = dpi();
  return (style_.padY * 2 + style_.thumbSide + style_.labelGap + style_.labelHeight) * d / 96;
}

int ThumbnailStrip::contentHeight() const {
  if (items_.empty()) return 0;
  const int gap = style_.itemGap * dpi() / 96;
  return itemCount() * itemHeight() + (itemCount() - 1) * gap;
}

int ThumbnailStrip::viewportHeight() const { return clientRect().height(); }

Rect ThumbnailStrip::itemRect(int index) const {
  if (index < 0 || index >= itemCount()) return Rect{};
  const int h = itemHeight();
  const int gap = style_.itemGap * dpi() / 96;
  const int top = index * (h + gap) - scrollOffset();
  return Rect{0, top, clientRect().right, top + h};
}

Size ThumbnailStrip::onMeasure(const Size& available) {
  // 尺寸完全由布局参数决定（固定宽、撑满高），内容长度不影响自身
  return Size{available.width, available.height};
}

// ───────────────────────── 绘制 ─────────────────────────

const Image& ThumbnailStrip::thumbnailFor(int index) {
  static const Image kInvalid;
  if (index < 0 || index >= itemCount()) return kInvalid;

  const std::wstring& path = items_[static_cast<size_t>(index)];
  auto it = thumbs_.find(path);
  if (it != thumbs_.end()) return it->second;

  // 按屏幕密度放大解码上限：150% 屏上 256 的图放进 126 像素的框会糊
  const int maxSide = style_.thumbMaxSide * dpi() / 96;
  Image image = platform()->loadImageThumbnail(path, maxSide);

  // 超上限就清掉不在当前视口里的项，留下刚解出来的这张
  if (thumbs_.size() >= kMaxCached) {
    const int first = (std::max)(0, scrollOffset() / (itemHeight() + px(style_.itemGap)));
    const int last = (std::min)(itemCount() - 1, first + viewportHeight() / itemHeight() + 2);
    for (auto iter = thumbs_.begin(); iter != thumbs_.end();) {
      bool visible = false;
      for (int i = first; i <= last; ++i) {
        if (items_[static_cast<size_t>(i)] == iter->first) {
          visible = true;
          break;
        }
      }
      if (visible) {
        ++iter;
      } else {
        if (iter->second.isValid()) platform()->releaseImage(iter->second);
        iter = thumbs_.erase(iter);
      }
    }
  }

  return thumbs_.emplace(path, image).first->second;
}

void ThumbnailStrip::releaseThumbnails() {
  for (auto& entry : thumbs_) {
    if (entry.second.isValid()) platform()->releaseImage(entry.second);
  }
  thumbs_.clear();
}

void ThumbnailStrip::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const Theme& t = theme();
  const Color textColor = style_.text != kNoColor ? style_.text : t.text;

  const int padX = style_.padX * d / 96;
  const int padY = style_.padY * d / 96;
  const int side = style_.thumbSide * d / 96;
  const int radius = style_.radius * d / 96;

  for (int i = 0; i < itemCount(); ++i) {
    const Rect item = itemRect(i);
    if (item.bottom <= rect.top) continue;
    if (item.top >= rect.bottom) break;

    const bool selected = i == selected_;
    const bool hot = i == hot_ && !selected;
    const Color bg = selected ? (style_.selBg != kNoColor ? style_.selBg : t.selBg)
                              : (hot ? (style_.itemHoverBg != kNoColor ? style_.itemHoverBg
                                                                      : t.rowHover)
                                     : (style_.itemBg != kNoColor ? style_.itemBg : kNoColor));

    const Rect box = item.deflated(padX / 2, 0);
    if (bg != kNoColor) painter.fillRoundRect(box, bg, radius);
    if (selected) {
      const Color border =
          style_.selBorder != kNoColor ? style_.selBorder : blendColor(t.selBg, t.accent, 0.55f);
      painter.strokeRoundRect(box, border, radius, (std::max)(1, d * 1 / 96));
    }

    // 图框固定为正方形，图按长边等比塞进去；空白处铺底色，避免透明图看成一团
    const int x = item.left + (item.width() - side) / 2;
    const int y = item.top + padY;
    const Rect frame{x, y, x + side, y + side};
    painter.fillRoundRect(frame, blendColor(t.pane, t.ink, 0.06f), radius / 2);

    const Image& image = thumbnailFor(i);
    if (image.isValid()) {
      const Size src = platform()->imageSize(image);
      if (src.width > 0 && src.height > 0) {
        const float k = (std::min)(static_cast<float>(side) / src.width,
                                   static_cast<float>(side) / src.height);
        const int w = (std::max)(1, static_cast<int>(src.width * k));
        const int h = (std::max)(1, static_cast<int>(src.height * k));
        const Rect dst{frame.left + (side - w) / 2, frame.top + (side - h) / 2,
                       frame.left + (side - w) / 2 + w, frame.top + (side - h) / 2 + h};
        painter.drawImage(image, dst);
      }
    }

    const int labelTop = frame.bottom + style_.labelGap * d / 96;
    const Rect labelRect{item.left + padX, labelTop, item.right - padX,
                         labelTop + style_.labelHeight * d / 96};
    painter.drawTextEllipsis(labelRect, fileNameOf(items_[static_cast<size_t>(i)]),
                             selected ? t.activeInk : textColor, fonts.uiSmall);
  }
}

Color ThumbnailStrip::backgroundColor() const {
  if (style_.background != kNoColor) return style_.background;
  return theme().tool;
}

void ThumbnailStrip::onScrollChanged() { invalidate(); }

// ───────────────────────── 交互 ─────────────────────────

int ThumbnailStrip::hitItem(Point local) const {
  if (local.y < 0 || local.y >= viewportHeight()) return -1;
  const int h = itemHeight();
  const int gap = px(style_.itemGap);
  const int index = (local.y + scrollOffset()) / (h + gap);
  if (index < 0 || index >= itemCount()) return -1;
  // 落在两行之间的间隙里不算命中
  if ((local.y + scrollOffset()) % (h + gap) >= h) return -1;
  return index;
}

bool ThumbnailStrip::onMouseDown(const MouseEvent& e) {
  // 滚动条优先：点在滑块或轨道上由基类处理
  if (ScrollView::onMouseDown(e)) return true;
  if (e.button != MouseButton::kLeft) return false;

  const int index = hitItem(e.position);
  if (index < 0) return false;

  setSelected(index);
  if (onSelect_) onSelect_(index);
  return true;
}

void ThumbnailStrip::onMouseMove(const MouseEvent& e) {
  ScrollView::onMouseMove(e);
  const int index = hitItem(e.position);
  if (index == hot_) return;
  // 只重画两条旧/新的行，缩略图条通常很窄，整块重画也不贵
  hot_ = index;
  invalidate();
}

void ThumbnailStrip::onMouseLeave() {
  ScrollView::onMouseLeave();
  if (hot_ < 0) return;
  hot_ = -1;
  invalidate();
}

void ThumbnailStrip::onDetach() {
  releaseThumbnails();
  ScrollView::onDetach();
}

}  // namespace AiUI
