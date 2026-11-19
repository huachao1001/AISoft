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

// 阴影层数：固定值，浓度只由 alpha 决定，见 paintShadow
constexpr int kShadowLayers = 6;

// 只取文件名部分：缩略图条宽度有限，目录前缀永远不会显示出来
std::wstring fileNameOf(const std::wstring& path) {
  const size_t pos = path.find_last_of(L"\\/");
  return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

}  // namespace

ThumbnailStrip::ThumbnailStrip() {
  // 缩略图条不需要横向滚动：宽度由布局定死，内容永远按宽度排
  setHorizontalScrollEnabled(false);
  // 卡片本身已经是一层底色，滚动条再画一条轨道会显得很脏
  ScrollBarStyle bar = ScrollView::style();
  bar.track = kNoColor;
  setScrollBarStyle(bar);
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
  cardStyle_ = style;
  requestLayout();
  invalidate();
}

void ThumbnailStrip::scrollToItem(int index) {
  if (index < 0 || index >= itemCount()) return;
  // 还没布局时视口高度是 0，任何一项都"看不见"，照公式算会把条目凭空滚出去一行。
  // 记下来等 onLayout 拿到真实高度再滚，否则打开靠后的图会停在列表顶部。
  const int viewport = viewportHeight();
  if (viewport <= 0) {
    pendingScrollTo_ = index;
    return;
  }
  pendingScrollTo_ = -1;

  const Rect rc = itemRect(index);
  if (rc.top < 0) {
    setScrollOffset(scrollOffset() + rc.top);
  } else if (rc.bottom > viewport) {
    setScrollOffset(scrollOffset() + rc.bottom - viewport);
  }
}

void ThumbnailStrip::onLayout(const Rect& frame) {
  ScrollView::onLayout(frame);
  if (pendingScrollTo_ < 0) return;
  const int index = pendingScrollTo_;
  pendingScrollTo_ = -1;
  // 这里必须重新调用：基类刚夹过滚动偏移，此时算出来的位置才是有效的
  scrollToItem(index);
}

// ───────────────────────── 几何 ─────────────────────────

int ThumbnailStrip::itemHeight() const {
  const int d = dpi();
  // 卡片只要容纳图与名称，本身不撑开；阴影画在卡片外，不占布局空间
  return (cardStyle_.cardPadY * 2 + cardStyle_.thumbH + cardStyle_.labelGap + cardStyle_.labelHeight) * d / 96;
}

int ThumbnailStrip::contentHeight() const {
  if (items_.empty()) return 0;
  const int d = dpi();
  const int pad = cardStyle_.sideMargin * d / 96;
  const int gap = cardStyle_.itemGap * d / 96;
  return pad * 2 + itemCount() * itemHeight() + (itemCount() - 1) * gap;
}

int ThumbnailStrip::viewportHeight() const { return clientRect().height(); }

Rect ThumbnailStrip::itemRect(int index) const {
  if (index < 0 || index >= itemCount()) return Rect{};
  const int d = dpi();
  const int h = itemHeight();
  const int gap = cardStyle_.itemGap * d / 96;
  const int left = cardStyle_.sideMargin * d / 96;
  const int top = cardStyle_.sideMargin * d / 96 + index * (h + gap) - scrollOffset();
  // 给滚动条让位：它是浮层，压住卡片右下角会很难看
  const int right = clientRect().right - left - scrollBarReserve();
  return Rect{left, top, (std::max)(left + 1, right), top + h};
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

  // 按屏幕密度放大解码上限：150% 屏上 256 的图放进 138 像素的框会糊
  const int maxSide = cardStyle_.thumbMaxSide * dpi() / 96;
  Image image = platform()->loadImageThumbnail(path, maxSide);

  // 超上限就清掉不在当前视口里的项，留下刚解出来的这张
  if (thumbs_.size() >= kMaxCached) {
    const int first = (std::max)(0, scrollOffset() / (itemHeight() + px(cardStyle_.itemGap)));
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

// Painter 只有纯色圆角矩形，没有模糊。这里由外到内叠若干层低透明度圆角：
// 外层扩得宽、内层扩得窄，越靠近卡片叠的层数越多，合起来就是一圈渐变。
// 层数固定，浓度只由 shadowAlpha 决定；层数随扩散像素走会让 1px 的屏
// 把全部浓度压在一条线上，反而发黑。
void ThumbnailStrip::paintShadow(Painter& painter, const Rect& card, int radius) const {
  const int d = dpi();
  const int spread = cardStyle_.shadowSpread * d / 96;
  if (spread <= 0 || cardStyle_.shadowAlpha <= 0) return;

  const int offsetY = cardStyle_.shadowOffsetY * d / 96;
  const int layers = kShadowLayers;
  for (int i = 1; i <= layers; ++i) {
    // i 越大越靠外：扩边量与外层透明度同步增长，边缘才不会一刀切
    const int grow = (spread * i + layers - 1) / layers;
    // 外层最淡、内层最浓，平方让浓度集中在贴近卡片的一小圈
    const float t = static_cast<float>(i) / static_cast<float>(layers);
    const std::uint8_t alpha = static_cast<std::uint8_t>(
        static_cast<float>(cardStyle_.shadowAlpha) * (1.0f - t) * (1.0f - t) + 0.5f);
    if (alpha == 0) continue;
    const Rect box = card.inflated(grow, grow).moved(0, offsetY);
    painter.fillRoundRect(box, makeColor(0x1a, 0x22, 0x30, alpha), radius + grow);
  }
}

void ThumbnailStrip::onPaint(Painter& painter, const Rect& rect) {
  const int d = dpi();
  const Theme& t = theme();
  const Color textColor = cardStyle_.text != kNoColor ? cardStyle_.text : t.text;

  const int padX = cardStyle_.cardPadX * d / 96;
  const int padY = cardStyle_.cardPadY * d / 96;
  const int radius = cardStyle_.cardRadius * d / 96;
  const int thumbH = cardStyle_.thumbH * d / 96;
  const int labelGap = cardStyle_.labelGap * d / 96;
  const int labelH = cardStyle_.labelHeight * d / 96;

  for (int i = 0; i < itemCount(); ++i) {
    const Rect card = itemRect(i);
    // 阴影会延伸到卡片之外，判可见范围时把扩散量算进去，否则上下边缘的
    // 阴影会在滚动时突然出现
    const int spread = cardStyle_.shadowSpread * d / 96;
    if (card.bottom + spread + spread <= rect.top) continue;
    if (card.top - spread - spread >= rect.bottom) break;

    const bool selected = i == selected_;
    const bool hot = i == hot_ && !selected;

    // 阴影先画，卡片底色再盖在上面，边缘那一圈才是卡片外沿
    paintShadow(painter, card, radius);

    // 选中不染整张卡片：卡片始终是白的，只靠描边和名称颜色区分，
    // 否则缩略图会被蓝底透过来的色带干扰
    const Color bg = hot ? (cardStyle_.cardHoverBg != kNoColor ? cardStyle_.cardHoverBg
                                                              : blendColor(t.pane, t.ink, 0.04f))
                         : (cardStyle_.cardBg != kNoColor ? cardStyle_.cardBg : t.pane);
    painter.fillRoundRect(card, bg, radius);

    if (selected) {
      const Color border =
          cardStyle_.selBorder != kNoColor ? cardStyle_.selBorder : t.accent;
      painter.strokeRoundRect(card, border, radius, (std::max)(2, d * 2 / 96));
    }

    // 图区：宽度撑满卡片内边，高度固定。图片按“覆盖”裁剪（fill）：让图铺满
    // 整个内衬，短边对齐、长边裁掉，卡片里看不到一圈空白。相册缩略图的
    // 第一眼是「这张图长什么样」，留白比裁掉一点边缘更干扰判断。
    const Rect frame{card.left + padX, card.top + padY, card.right - padX,
                     card.top + padY + thumbH};
    if (frame.width() > 0) {
      const int innerRadius = (std::max)(2, radius / 2);
      // 内衬只在图片缺席时可见，有图时图片会把它完全盖住
      painter.fillRoundRect(frame, blendColor(t.pane, t.ink, 0.09f), innerRadius);

      const Image& image = thumbnailFor(i);
      if (image.isValid()) {
        const Size src = platform()->imageSize(image);
        if (src.width > 0 && src.height > 0) {
          // 取较大的缩放比 = 覆盖；较小的 = 完整显示但留白
          const float k = (std::max)(static_cast<float>(frame.width()) / src.width,
                                     static_cast<float>(frame.height()) / src.height);
          const int w = (std::max)(1, static_cast<int>(src.width * k));
          const int h = (std::max)(1, static_cast<int>(src.height * k));
          const Rect dst{frame.left + (frame.width() - w) / 2,
                         frame.top + (frame.height() - h) / 2,
                         frame.left + (frame.width() - w) / 2 + w,
                         frame.top + (frame.height() - h) / 2 + h};
          // 裁到内衬范围：覆盖模式下长边一定会溢出
          painter.save();
          painter.clipRect(frame);
          painter.drawImage(image, dst);
          painter.restore();
        }
      }
    }

    // 名称居中：卡片里图是居中的，文字靠左会显得歪
    const int labelTop = frame.bottom + labelGap;
    const Rect labelRect{card.left + padX, labelTop, card.right - padX, labelTop + labelH};
    const std::wstring name = fileNameOf(items_[static_cast<size_t>(i)]);
    const Color nameColor = selected ? t.activeInk : textColor;
    if (painter.measureText(name, fonts.uiSmall) <= labelRect.width()) {
      painter.drawText(labelRect, name, nameColor, fonts.uiSmall, TextAlign::kCenter);
    } else {
      // 放不下时退回左对齐省略：居中裁掉尾部会两边都看不全
      painter.drawTextEllipsis(labelRect, name, nameColor, fonts.uiSmall);
    }
  }
}

Color ThumbnailStrip::backgroundColor() const {
  if (cardStyle_.background != kNoColor) return cardStyle_.background;
  return theme().tool;
}

void ThumbnailStrip::onScrollChanged() { invalidate(); }

// ───────────────────────── 交互 ─────────────────────────

int ThumbnailStrip::hitItem(Point local) const {
  if (local.y < 0 || local.y >= viewportHeight()) return -1;
  const int d = dpi();
  const int h = itemHeight();
  const int gap = cardStyle_.itemGap * d / 96;
  // 去掉顶部留白再折算序号
  const int y = local.y + scrollOffset() - cardStyle_.sideMargin * d / 96;
  if (y < 0) return -1;
  const int index = y / (h + gap);
  if (index < 0 || index >= itemCount()) return -1;
  // 落在卡片之间的间隙里不算命中
  if (y % (h + gap) >= h) return -1;
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
  // 只重画两条旧/新的卡片，缩略图条通常很窄，整块重画也不贵
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
