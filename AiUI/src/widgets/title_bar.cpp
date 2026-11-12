// TitleBar 实现：通用控件拼装 + 纯交互
#include "AiUI/widgets/title_bar.h"

#include "AiUI/layouts/linear_layout.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/image_view.h"
#include "AiUI/widgets/text_view.h"

#include <algorithm>

namespace AiUI {
namespace {

// 系统按钮一律无边框、只有悬停底色；关闭按钮悬停变红
ButtonSpec makeButtonSpec(const TitleBarStyle& style, const TitleBarDefaults& defaults,
                          IconId icon, bool close) {
  ButtonSpec spec;
  spec.style = ButtonStyle::kFlat;
  spec.placement = IconPlacement::kOnly;
  spec.hasIcon = icon != IconId::kNone;
  spec.icon = icon;
  spec.iconSize = defaults.iconSize;
  spec.radius = 0;
  spec.padX = 0;
  spec.width = defaults.buttonWidth;
  spec.height = defaults.bandHeight;
  // 系统标题栏按钮配箭头光标，不是手形
  spec.cursor = CursorShape::kArrow;
  spec.background = kNoColor;
  spec.border = kNoColor;
  spec.hoverBackground = close ? style.closeHover : style.buttonHover;
  spec.downBackground = close ? style.closeDown : style.buttonDown;
  spec.hoverInk = close ? style.closeInk : kNoColor;
  return spec;
}

}  // namespace

TitleBar::TitleBar() : defaults_(titleBarDefaults()) {
  setId(kTitleBarIdRoot);
  setFocusable(false);
  bandHeightOverride_ = defaults_.bandHeight;

  band_ = new LinearLayout(Orientation::kHorizontal);
  band_->setId(kTitleBarIdBand);
  // 交叉轴居中：图标、标题、按钮都按逻辑带垂直居中
  band_->setGravity(Gravity::kCenter);
  addView(band_,
          LayoutParams{}.withSize(LayoutParams::kMatchParent, defaults_.bandHeight));

  auto makeIcon = [this] {
    iconView_ = new ImageView();
    iconView_->setId(kTitleBarIdIcon);
    iconView_->setScale(ImageScale::kFit);
    return iconView_;
  };
  auto makeTitle = [this] {
    titleView_ = new TextView();
    titleView_->setId(kTitleBarIdTitle);
    return titleView_;
  };
  auto makeButtons = [this] {
    buttons_ = new LinearLayout(Orientation::kHorizontal);
    buttons_->setId(kTitleBarIdButtons);
    buttons_->setGravity(Gravity::kCenter);
    return buttons_;
  };
  auto makeButton = [this](int id, IconId icon, bool close, Handler TitleBar::*handler) {
    auto* button = new Button();
    button->setId(id);
    button->setSpec(makeButtonSpec(style_, defaults_, icon, close));
    button->setOnClick([this, handler] {
      if (this->*handler) (this->*handler)();
    });
    buttons_->addView(button, LayoutParams{}.withSize(defaults_.buttonWidth,
                                                      LayoutParams::kMatchParent));
    return button;
  };

  auto* icon = makeIcon();
  auto* title = makeTitle();
  auto* buttons = makeButtons();
  minimizeButton_ =
      makeButton(kTitleBarIdMinimize, IconId::kMinimize, false, &TitleBar::onMinimize_);
  maximizeButton_ =
      makeButton(kTitleBarIdMaximize, IconId::kMaximize, false, &TitleBar::onToggleMaximize_);
  closeButton_ = makeButton(kTitleBarIdClose, IconId::kClose, true, &TitleBar::onClose_);

  // 占位把两侧撑开：按钮在哪一侧，另一端就是弹簧
  auto* spacer = new View();
  const LayoutParams spacerParams = LayoutParams{}.withSize(0, 0).withWeight(1.0f);

  const LayoutParams iconParams =
      LayoutParams{}.withSize(defaults_.appIconSize, LayoutParams::kWrapContent);
  const LayoutParams titleParams =
      LayoutParams{}.withSize(LayoutParams::kWrapContent, LayoutParams::kMatchParent);
  const LayoutParams buttonsParams =
      LayoutParams{}.withSize(LayoutParams::kWrapContent, LayoutParams::kMatchParent);

  if (defaults_.buttonsOnLeft) {
    band_->addView(buttons, buttonsParams);
    band_->addView(spacer, spacerParams);
    band_->addView(icon, iconParams);
    band_->addView(title, titleParams);
  } else {
    band_->addView(icon, iconParams);
    band_->addView(title, titleParams);
    band_->addView(spacer, spacerParams);
    band_->addView(buttons, buttonsParams);
  }
  band_->setPadding(defaults_.padLeft, 0, defaults_.padRight, 0);

  // 整条逻辑带都可拖动窗口
  band_->setDragWindow(true);

  applyStyle();
  syncButtonIcons();
}

TitleBar::~TitleBar() = default;

// ───────────────────────── 外观 ─────────────────────────

void TitleBar::setStyle(const TitleBarStyle& style) {
  style_ = style;
  // applyStyle 会顺手改图标的右边距，间距变了要重新排布
  applyStyle();
  requestLayout();
  invalidate();
}

void TitleBar::setHeight(int height) {
  if (heightOverride_ == height) return;
  heightOverride_ = height;
  requestLayout();
  invalidate();
}

int TitleBar::height() const {
  return heightOverride_ > 0 ? heightOverride_ : bandHeight();
}

void TitleBar::setBandHeight(int height) {
  if (bandHeightOverride_ == height) return;
  bandHeightOverride_ = height;

  if (band_) {
    LayoutParams lp = band_->layoutParams();
    lp.height = height;
    band_->setLayoutParams(lp);
  }
  // 按钮占满整条逻辑带，悬停底色才会与逻辑高度一致
  for (Button* button : {minimizeButton_, maximizeButton_, closeButton_}) {
    if (!button) continue;
    ButtonSpec spec = button->spec();
    spec.height = height;
    button->setSpec(spec);
  }
  requestLayout();
  invalidate();
}

int TitleBar::bandHeight() const { return bandHeightOverride_; }

void TitleBar::setTitle(const std::wstring& title) {
  if (title_ == title) return;
  title_ = title;
  if (titleView_) titleView_->setText(title_);
  requestLayout();
  invalidate();
}

void TitleBar::setAppIcon(Image icon) {
  appIcon_ = icon;
  if (iconView_) {
    if (icon.isValid()) iconView_->setImage(icon);
    else iconView_->clearImage();
  }
  applyStyle();
  requestLayout();
  invalidate();
}

void TitleBar::setTitleIconVisible(bool visible) {
  if (showTitleIcon_ == visible) return;
  showTitleIcon_ = visible;
  applyStyle();
  requestLayout();
  invalidate();
}

void TitleBar::setMinimizable(bool v) {
  if (minimizable_ == v) return;
  minimizable_ = v;
  if (minimizeButton_) minimizeButton_->show(v);
  requestLayout();
  invalidate();
}

void TitleBar::setMaximizable(bool v) {
  if (maximizable_ == v) return;
  maximizable_ = v;
  if (maximizeButton_) maximizeButton_->show(v);
  requestLayout();
  invalidate();
}

void TitleBar::setMaximized(bool v) {
  if (maximized_ == v) return;
  maximized_ = v;
  syncButtonIcons();
}

void TitleBar::setActive(bool v) {
  if (active_ == v) return;
  active_ = v;
  applyStyle();
  invalidate();
}

Button* TitleBar::systemButton(TitleBarButton button) const {
  switch (button) {
    case TitleBarButton::kMinimize: return minimizeButton_;
    case TitleBarButton::kMaximize: return maximizeButton_;
    case TitleBarButton::kClose: return closeButton_;
    default: return nullptr;
  }
}

// 样式与激活状态统一下发到各部件
void TitleBar::applyStyle() {
  // kNoColor 落到主题：标题栏不该写死白色，深色主题下会刺眼
  const Theme& t = theme();
  TitleBarStyle s = style_;
  if (s.background == kNoColor) s.background = t.titleBg;
  if (s.activeBackground == kNoColor) s.activeBackground = t.titleBg;
  if (s.text == kNoColor) s.text = t.titleText;
  if (s.inactiveText == kNoColor) s.inactiveText = t.titleTextInactive;
  if (s.buttonHover == kNoColor) s.buttonHover = t.titleButtonHover;
  if (s.buttonDown == kNoColor) s.buttonDown = t.titleButtonDown;
  if (s.closeHover == kNoColor) s.closeHover = t.closeHover;
  if (s.closeDown == kNoColor) s.closeDown = t.closeDown;
  if (s.closeInk == kNoColor) s.closeInk = t.closeInk;
  if (s.line == kNoColor) s.line = t.titleLine;

  const Color ink = active_ ? s.text : s.inactiveText;

  if (iconView_) {
    const bool show = showTitleIcon_ && appIcon_.isValid() && defaults_.appIconSize > 0;
    iconView_->show(show);
    // 图标与标题之间留一点间隙，不显示图标时由标题自己带左边距
    LayoutParams lp = iconView_->layoutParams();
    lp.marginRight = show ? defaults_.iconGap : 0;
    iconView_->setLayoutParams(lp);
  }

  if (titleView_) {
    TextStyle textStyle;
    textStyle.color = ink;
    textStyle.ellipsis = true;
    titleView_->setTextStyle(textStyle);
  }

  for (Button* button : {minimizeButton_, maximizeButton_, closeButton_}) {
    if (!button) continue;
    const bool isClose = button == closeButton_;
    ButtonSpec spec = makeButtonSpec(s, defaults_, button->spec().icon, isClose);
    spec.height = bandHeightOverride_;
    spec.ink = ink;
    spec.hoverInk = isClose ? s.closeInk : ink;
    button->setSpec(spec);
  }

  setBackgroundColor(active_ ? s.activeBackground : s.background);
}

void TitleBar::syncButtonIcons() {
  if (maximizeButton_) {
    maximizeButton_->setIcon(maximized_ ? IconId::kRestore : IconId::kMaximize);
  }
}

// ───────────────────────── 布局 ─────────────────────────

Size TitleBar::onMeasure(const Size& available) {
  return Size{available.width, px(height())};
}

// 子视图摆放交给 FrameLayout：band 的高度写在自身的布局参数里，
// 贴顶靠默认的左上对齐；超出标题栏的部分由 View::draw 裁剪。

// ───────────────────────── 绘制 ─────────────────────────

void TitleBar::onPaint(Painter& painter, const Rect& rect) {
  if (style_.lineThickness <= 0) return;
  // 底部分隔线，与内容区分开
  const int thickness = (std::max)(1, style_.lineThickness * dpi() / 96);
  const int top = rect.bottom - thickness;
  const Color line = style_.line != kNoColor ? style_.line : theme().titleLine;
  painter.fillRect(Rect{rect.left, top, rect.right, rect.bottom}, line);
}

// ───────────────────────── 交互 ─────────────────────────

HitTarget TitleBar::onHitTest(const Point& windowPoint) {
  // 最大化按钮交给系统，才能弹出贴靠浮层
  if (maximizable_ && maximizeButton_ && maximizeButton_->isVisible() &&
      maximizeButton_->absoluteBounds().contains(windowPoint)) {
    return HitTarget::kMaxButton;
  }
  // 其余按钮要收客户区事件，否则点不到
  for (Button* button : {minimizeButton_, maximizeButton_, closeButton_}) {
    if (button && button->isVisible() && button->absoluteBounds().contains(windowPoint)) {
      return HitTarget::kClient;
    }
  }
  // 逻辑带内可拖动窗口；带外只有显式开了 dragWindow 的视图才算
  if (band_ && band_->absoluteBounds().contains(windowPoint)) return HitTarget::kCaption;

  for (auto it = children().rbegin(); it != children().rend(); ++it) {
    View* child = *it;
    if (!child->isVisible() || child == band_) continue;
    if (!child->absoluteBounds().contains(windowPoint)) continue;
    return child->isDragWindow() ? HitTarget::kCaption : HitTarget::kClient;
  }
  return HitTarget::kClient;
}

}  // namespace AiUI
