#include "pyui/widgets/title_bar.h"

#include "pyui/layout_inflater.h"
#include "pyui/platform/window.h"

#include <algorithm>

namespace pyui {
namespace {

// 把外层容器的子视图搬进 TitleBar 自己。
//
// 为什么需要这一步：内置布局的根节点是 <FrameLayout id="0x7A00">，而
// TitleBar 自己就是那个 FrameLayout 的子类。直接 new 一个 FrameLayout 再
// 把它当作根，会让树里多一层没有任何作用的容器（TitleBar → FrameLayout
// → band），absoluteBounds 与命中测试都要多走一层，而且调用方
// findView(TitleBar) 拿到的对象与布局根不是同一个。
//
// 所以这里只取根的子视图，丢掉根容器本身：结构仍然是
// 「TitleBar(FrameLayout) → band」，与 AiUI 一致。
//
// children_ 是 View 的公开成员，先清空再 addView：addView 会检查 parent_，
// 若还指向旧容器就会调 removeView（它会 delete），所以必须先断开旧容器的
// 子表。
void adoptChildren(ViewGroup& from, ViewGroup& to) {
  std::vector<View*> moved = from.children_;
  std::vector<LayoutParams> params;
  params.reserve(moved.size());
  for (View* child : moved) params.push_back(child->layoutParams());

  from.children_.clear();
  for (size_t i = 0; i < moved.size(); ++i) {
    to.addView(moved[i], params[i]);
  }
}

}  // namespace

// ── 构造 ──

TitleBar::TitleBar() {
  appearance_ = titleBarAppearance();
  setId(kIdRoot);

  // 结构来自布局。内置的 titlebar.xml 始终注册（见 CMake 的 pyui_builtin_layouts），
  // 应用放同名文件就会把注册表里那一项覆盖掉——LayoutInflater::add 是
  // 「后写覆盖先写」，而应用自己的注册发生在静态初始化期，顺序由链接顺序
  // 保证不了，所以这里在**运行期**再查一次：如果应用注册了同名布局，
  // inflate 拿到的就是应用那份。
  // 标题栏整体就是窗口的拖动条。声明成角色而不是靠下面 onHitTest 的兜底，
  // 是为了让「命中的是被替换过的 this」时角色信息不丢——hitTest 会把 band
  // 上的命中改判给 TitleBar 自己，若这里没有角色，XML 里写的 role="caption"
  // 就在这一步被丢掉了。
  setWindowRole(WindowRole::kCaption);
  std::unique_ptr<ViewGroup> root = LayoutInflater::inflate(kDefaultLayout);
  if (root == nullptr) {
    // 布局没注册也不该让标题栏变成空条：至少保住三个按钮，窗口还能用。
    // 这种情况只会在「有人把内置布局从构建里去掉」时出现。
    return;
  }
  adoptChildren(*root, *this);

  if (!bindParts()) {
    // 布局被改到认不出必要部件：保留布局本身（用户可能故意换了结构），
    // 但不要再对空指针做任何事。窗口仍然能显示与拖动。
    return;
  }

  minimize_->setOnClick([this] {
    if (window_ != nullptr) window_->minimize();
  });
  maximize_->setOnClick([this] {
    if (window_ != nullptr) window_->toggleMaximize();
  });
  close_->setOnClick([this] {
    // 拦截器返回 true 表示「还有未保存内容，先别关」
    if (onCloseRequest_ && onCloseRequest_()) return;
    if (window_ != nullptr) window_->close();
  });

  // 图标、标题与占位都是装饰：不参与命中，事件落到它们所在的容器，
  // 再经 TitleBar::hitTest 回落到标题栏自己——拖动窗口与双击最大化
  // 才对整条标题栏有效（包括标题文字与空白区）。
  //
  // 注意 band 本身**不能**透明：透明会让整个子树（含三个按钮）一起跳过，
  // 关闭按钮就点不到了。这里只标记叶子装饰。
  iconView_->setMouseTransparent(true);
  titleView_->setMouseTransparent(true);
  if (View* spacer = findView(kIdSpacer)) spacer->setMouseTransparent(true);

  // 标题栏按钮不抢焦点：Tab 在客户区控件之间走，落到关闭按钮上会误关窗口。
  // 布局 XML 里没有这个属性（它是行为而非外观），所以在这里统一设置。
  for (Button* button : {minimize_, maximize_, close_}) button->setFocusable(false);

  updateButtonStates();
  updatePalette();
}

TitleBar::~TitleBar() = default;

bool TitleBar::bindParts() {
  band_ = findView<ViewGroup>(kIdBand);
  buttonStrip_ = findView<ViewGroup>(kIdButtons);

  // 按钮按 role 找，id 只作为兼容回退。这样应用自定义 titlebar.xml 时
  // 把按钮换个 id、挪到别处，只要写了 role 就仍然能接上。
  minimize_ = findByRoleAs<Button>(WindowRole::kMinimize);
  maximize_ = findByRoleAs<Button>(WindowRole::kMaximize);
  close_ = findByRoleAs<Button>(WindowRole::kClose);
  if (minimize_ == nullptr) minimize_ = findView<Button>(kIdMinimize);
  if (maximize_ == nullptr) maximize_ = findView<Button>(kIdMaximize);
  if (close_ == nullptr) close_ = findView<Button>(kIdClose);

  iconView_ = findByRoleAs<ImageView>(WindowRole::kIcon);
  titleView_ = findByRoleAs<TextView>(WindowRole::kTitle);
  if (iconView_ == nullptr) iconView_ = findView<ImageView>(kIdIcon);
  if (titleView_ == nullptr) titleView_ = findView<TextView>(kIdTitle);

  return band_ != nullptr && iconView_ != nullptr && titleView_ != nullptr &&
         buttonStrip_ != nullptr && minimize_ != nullptr && maximize_ != nullptr &&
         close_ != nullptr;
}

// 在子树里按窗口角色找控件。角色来自 XML 的 role 属性，与 id、位置无关。
View* TitleBar::findByRole(WindowRole role) {
  // 深度优先，取第一个匹配。用显式栈而不是递归，标题栏子树的深度很小，
  // 但这里没有理由让一个固定布局去消耗调用栈。
  std::vector<View*> stack{this};
  while (!stack.empty()) {
    View* view = stack.back();
    stack.pop_back();
    if (view != this && view->windowRole() == role) return view;
    if (auto* group = dynamic_cast<ViewGroup*>(view)) {
      for (View* child : group->children()) stack.push_back(child);
    }
  }
  return nullptr;
}

void TitleBar::setWindow(PlatformWindow* window) {
  window_ = window;
  // 绑定窗口时同步一次初值：窗口可能已经处于最大化（还原会话时）
  if (window_ != nullptr) maximized_ = window_->isMaximized();
  updateButtonStates();
}

void TitleBar::setTheme(const Theme* theme) {
  View::setTheme(theme);
  // 标题色与按钮配色是缓存值，换肤后必须按新主题重算一遍
  updatePalette();
}

// ── 外观 ──

void TitleBar::setAppearance(const TitleBarAppearance& appearance) {
  appearance_ = appearance;
  updateButtonStates();
  updatePalette();
  requestLayout();
  invalidate();
}

void TitleBar::setTitle(std::string title) {
  if (titleView_ != nullptr) titleView_->setText(std::move(title));
}

const std::string& TitleBar::title() const {
  static const std::string kEmpty;
  return titleView_ != nullptr ? titleView_->text() : kEmpty;
}

void TitleBar::setActive(bool active) {
  if (active_ == active) return;
  active_ = active;
  updatePalette();
  invalidate();
}

void TitleBar::setIcon(const Image& image) {
  icon_ = image;
  if (iconView_ != nullptr) iconView_->setImage(image);
  invalidate();
}

void TitleBar::setIconResourceId(int resourceId) {
  // 资源 id 由平台后端的图标加载器解释；核心层不碰资源系统，
  // 后端取到图像后调 setIcon。这里刻意不做「假装加载」。
  (void)resourceId;
}

// ── 配色 ──

namespace {

// 主题是否被显式替换过。默认主题与平台标题栏外观是同一套数字，
// 所以「没换主题」时用 appearance 里的平台基准值，换了主题就整体跟随主题，
// 否则深色主题下会出现一条刺白的标题栏。
bool usesCustomTheme(const Theme& theme) { return &theme != &defaultTheme(); }

}  // namespace

void TitleBar::updatePalette() {
  const Theme& t = theme();
  const bool themed = usesCustomTheme(t);

  const Color background =
      active_ ? (themed ? t.titleBg : appearance_.background)
              : (themed ? t.titleBgInactive : appearance_.backgroundInactive);
  const Color ink = active_ ? (themed ? t.titleText : appearance_.titleInk)
                            : (themed ? t.titleTextInactive : appearance_.titleInkInactive);
  const Color hover = themed ? t.titleButtonHover : appearance_.buttonHover;
  const Color down = themed ? t.titleButtonDown : appearance_.buttonDown;
  const Color closeHover = themed ? t.closeHover : appearance_.closeHover;
  const Color closeDown = themed ? t.closeDown : appearance_.closeDown;
  const Color closeInk = themed ? t.closeInk : appearance_.closeInk;

  setBackgroundColor(background);

  // 非激活时标题变淡，这是用户判断「哪个窗口接收键盘」的主要线索
  if (titleView_ != nullptr) {
    titleView_->setColor(ink);
    // 标题档位来自 appearance：Windows/macOS 都是 9pt，少数 Linux 主题更大
    if (appearance_.titlePointSize > 12.0f) {
      titleView_->setTextSize(TextSize::kHuge);
    } else if (appearance_.titlePointSize > 9.5f) {
      titleView_->setTextSize(TextSize::kLarge);
    } else if (appearance_.titlePointSize < 8.5f) {
      titleView_->setTextSize(TextSize::kSmall);
    } else {
      titleView_->setTextSize(TextSize::kNormal);
    }
    titleView_->setBold(appearance_.titleBold);
  }

  if (iconView_ != nullptr) iconView_->setIconColor(ink);

  const int designWidth =
      appearance_.roundButtons ? appearance_.buttonHeight : appearance_.buttonWidth;
  const int designHeight = appearance_.buttonHeight;
  // 按钮图标与应用图标用同一个尺寸：两者都在标题栏里，一样大才不会
  // 让人觉得「窗口图标比按钮里的字形重一档」。
  // macOS 的交通灯是小圆点，另有一套更小的尺寸。
  const int iconSize = appearance_.roundButtons ? 8 : appearance_.iconSize;
  const int radius = appearance_.roundButtons ? designHeight / 2 : 0;

  auto configure = [&](Button* button, Color hoverColor, Color downColor, Color idleInk,
                       Color hotInk) {
    if (button == nullptr) return;
    ButtonSpec spec = button->spec();
    spec.width = designWidth;
    spec.height = designHeight;
    spec.iconSize = iconSize;
    spec.radius = radius;
    spec.style = ButtonStyle::kSubtle;
    spec.placement = IconPlacement::kOnly;
    spec.padX = 0;
    spec.cursor = CursorShape::kArrow;
    spec.ink = idleInk;
    spec.background = kNoColor;
    spec.hoverBackground = hoverColor;
    spec.downBackground = downColor;
    spec.hoverInk = hotInk;
    button->setSpec(spec);
  };

  configure(minimize_, hover, down, ink, ink);
  configure(maximize_, hover, down, ink, ink);
  // 关闭按钮悬停时整块变红、字形转白：这是 Windows 的既有习惯，
  // 也是「误点代价最高」的按钮必须给出的强提示。
  configure(close_, closeHover, closeDown, ink, closeInk);
}

void TitleBar::updateButtonStates() {
  // 最大化后按钮含义变成「还原」，图标与点击结果必须一起换，
  // 只换图标会让用户以为还能继续最大化。
  if (maximize_ != nullptr) {
    maximize_->setIcon(maximized_ ? IconId::kRestore : IconId::kMaximize);
  }
}

void TitleBar::setMaximized(bool maximized) {
  if (maximized_ == maximized) return;
  maximized_ = maximized;
  updateButtonStates();
}

// ── 布局 ──

Size TitleBar::onMeasure(const Size& available) {
  // 高度由外观决定，宽度撑满：
  //   bandHeight 是内容与按钮的对齐基准，visualHeight 多出的部分是宿主
  //   自己叠工具栏底纹用的。所以高度取 effectiveVisualHeight 而不是 band。
  int width = available.width > 0 ? available.width : px(300);
  int height = px(appearance_.effectiveVisualHeight());
  if (available.height > 0) height = std::min(height, available.height);
  return Size{width, height};
}

void TitleBar::onLayout(const Rect& frame) {
  (void)frame;
  if (band_ == nullptr) return;

  const int bandHeight = px(appearance_.bandHeight);
  // band 贴在顶部，高度取逻辑高度；视觉高度多出来的部分留给 onPaint 的背景。
  const Rect bandRect{0, 0, bounds().width(), std::min(bounds().height(), bandHeight)};

  // 先让 band 按自己的布局参数摆一遍子视图（XML 里写了顺序与权重），
  // 再按平台度量精确覆盖：圆点按钮、按钮条靠哪一侧、图标尺寸全在
  // arrangeBand 里算，静态布局表达不了。
  band_->layout(bandRect);
  arrangeBand(bandRect);
}

void TitleBar::arrangeBand(const Rect& band) {
  if (band_ == nullptr) return;

  const int inset = px(appearance_.contentInset);
  const int gap = px(appearance_.iconTextGap);
  const bool showIcon = iconView_ != nullptr && appearance_.appIconSize > 0;
  const int iconSize = showIcon ? px(appearance_.appIconSize) : 0;

  const int designWidth =
      appearance_.roundButtons ? appearance_.buttonHeight : appearance_.buttonWidth;
  const int gapBetweenButtons = px(appearance_.roundButtons ? 8 : 0);
  const int stripWidth = 3 * px(designWidth) + 2 * gapBetweenButtons;

  // 按钮条贴哪一侧由平台决定（macOS 在左，Windows/Linux 在右）。
  // 这里只摆按钮条这个容器，条内三个按钮的顺序由 arrangeChildren 决定，
  // 所以「红绿灯在最左」不需要改任何坐标。
  Rect strip;
  if (appearance_.buttonsOnSide == TitleButtonSide::kRight) {
    strip = Rect{band.right - stripWidth, band.top, band.right, band.bottom};
  } else {
    strip = Rect{band.left, band.top, band.left + stripWidth, band.bottom};
  }
  if (buttonStrip_ != nullptr) {
    buttonStrip_->layout(strip);
    // 按钮的**顺序**也由平台决定：Windows/Linux 是「最小化、最大化、关闭」，
    // macOS 是「关闭、最小化、最大化」（红绿灯）。XML 里写的是一种顺序
    // （Windows 习惯），这里按 appearance 重新摆位置——不重排子视图、
    // 不另写一份 XML，只覆盖坐标。
    const int step = px(designWidth) + gapBetweenButtons;
    // 子视图坐标相对按钮条自身的左上角（View::layout 的 frame 是父容器局部坐标）
    const int offsetY = (strip.height() - px(appearance_.buttonHeight)) / 2;
    Button* buttons[3] = {minimize_, maximize_, close_};
    for (int i = 0; i < 3; ++i) {
      Button* button = buttons[i];
      if (button == nullptr) continue;
      int slot = i;
      if (appearance_.buttonsOnSide == TitleButtonSide::kLeft) {
        // 关闭占 0 号槽，其余顺延。简单反转会得到「关闭、最大化、最小化」，
        // 那是错的：macOS 中间是最小化。
        slot = i == 2 ? 0 : i + 1;
      }
      button->layout(Rect::at(slot * step, offsetY, px(designWidth),
                              px(appearance_.buttonHeight)));
    }
  }

  int contentLeft = band.left + inset;
  int contentRight = band.right - inset;

  if (appearance_.buttonsOnSide == TitleButtonSide::kRight) {
    contentRight = strip.left - gap;
  } else {
    contentLeft = strip.right + gap;
  }

  if (iconView_ != nullptr) {
    if (showIcon) {
      const int top = band.top + (band.height() - iconSize) / 2;
      iconView_->layout(Rect{contentLeft, top, contentLeft + iconSize, top + iconSize});
      // 间距只在真的画了图标时才留：macOS 不显示应用图标，多留一截会让
      // 标题凭空右移 8 + 18 像素。
      contentLeft += iconSize + gap;
    } else {
      iconView_->layout(Rect{});
    }
  }

  if (titleView_ != nullptr) {
    const int titleLeft = std::min(contentLeft, contentRight);
    titleView_->layout(Rect{titleLeft, band.top, std::max(titleLeft, contentRight), band.bottom});
  }
}

// ── 绘制 ──

void TitleBar::onPaint(Painter& painter, const Rect& rect) {
  if (rect.isEmpty()) return;

  const Theme& t = theme();
  const bool themed = usesCustomTheme(t);

  const Color bandBottomLine = themed ? t.titleLine : appearance_.bottomLine;

  // 底色由 View::draw 按 resolveBackground 铺满整个 bounds（含视觉高度的
  // 多出部分），这里只补标题栏特有的东西——重复铺一次是纯浪费。
  // 应用图标由 ImageView 自己画，不在这里。
  const int bandBottom = rect.top + px(appearance_.bandHeight);
  if (appearance_.drawBottomLine && bandBottom < rect.bottom) {
    const int width = std::max(1, px(1));
    painter.fillRect(Rect{rect.left, bandBottom, rect.right, bandBottom + width}, bandBottomLine);
  }
}

// ── 鼠标 ──

View* TitleBar::hitTest(Point localPoint) const {
  View* hit = ViewGroup::hitTest(localPoint);
  // 落在 band 自身（而不是某个按钮）时，把事件交给 TitleBar：
  // 拖动窗口与双击最大化的语义属于整条标题栏，而不是内部容器。
  if (hit == band_) return const_cast<TitleBar*>(this);
  return hit;
}

bool TitleBar::onMouseDown(const MouseEvent& e) {
  if (e.button != MouseButton::kLeft) return false;
  // 只有空白区会走到这里：三个按钮被命中时事件直接投给按钮
  if (window_ != nullptr) window_->startDragMove();
  return true;
}

void TitleBar::onDoubleClick(const MouseEvent& e) {
  (void)e;
  if (onDoubleClickBlank_) {
    onDoubleClickBlank_();
    return;
  }
  if (window_ != nullptr) window_->toggleMaximize();
}

}  // namespace pyui
