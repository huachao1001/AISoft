#include "pyui/app_window.h"

#include "pyui/canvas.h"
#include "pyui/layout_inflater.h"

#include <algorithm>

namespace pyui {

AppWindow::AppWindow(Options options) : options_(std::move(options)) {
  // 内置布局（默认标题栏）必须先注册：标题栏的结构来自 titlebar.xml，
  // 而它编译进 pyui_core 静态库，静态注册项可能被链接器丢掉。
  // 这里显式调一次，任何应用都不用自己记得做这件事。
  registerBuiltinLayouts();

  root_.setTheme(theme_);
  // 脏区 → 窗口失效。这一条是「控件改了属性就一定能看见」的唯一保证：
  // 控件的 invalidate 里没有窗口指针，只能通过 ViewRoot 回调出去。
  root_.setInvalidateHandler([this](const Rect& dirty) {
    if (window_ != nullptr && !closing_) window_->invalidateRect(dirty);
  });
}

AppWindow::~AppWindow() {
  // 先拆内容再销毁窗口：视图的 onDetached 里可能还会碰 root_，
  // 而窗口一旦销毁，后端就不再回调宿主，但画布缓存的释放要等这里。
  root_.setContentView(nullptr);
  pendingContent_.reset();
  if (window_ != nullptr) {
    platform().destroyWindow(window_);
    window_ = nullptr;
  }
}

// ── 启动 ──

Rect AppWindow::centeredBounds(int dpi) const {
  // 设计值 → 物理像素。用 root_ 的换算而不是自己写一遍：两处的取整规则
  // 必须一致，否则 125% 屏上窗口会比内容宽出半个像素并出现一条缝。
  const int screenDpi = dpi > 0 ? dpi : 96;
  auto scale = [screenDpi](int value) {
    if (value == 0) return 0;
    const long long scaled = static_cast<long long>(value) * screenDpi;
    return static_cast<int>((scaled + 48) / 96);
  };

  const Rect work = platform().workArea();
  int width = scale(options_.width);
  int height = scale(options_.height);
  // 屏幕太小时收敛到工作区，不把按钮挤出屏幕外
  if (work.width() > 0) width = std::min(width, work.width());
  if (work.height() > 0) height = std::min(height, work.height());

  const int x = work.left + (work.width() - width) / 2;
  const int y = work.top + (work.height() - height) / 2;
  return Rect::at(x, y, width, height);
}

bool AppWindow::create() {
  if (window_ != nullptr) return true;

  const int screenDpi = platform().screenDpi();

  WindowParams params;
  params.visible = false;
  params.title = options_.title;
  params.className = options_.className;
  params.iconResourceId = options_.iconResourceId;
  params.minWidth = options_.minWidth;
  params.minHeight = options_.minHeight;
  params.bounds = centeredBounds(screenDpi);
  params.style.topLevel = true;
  params.style.resizable = options_.resizable;
  params.style.minimizable = options_.minimizable;
  params.style.maximizable = options_.maximizable;

  if (options_.customTitleBar) {
    // captionHeight 用的是 TitleBarAppearance 的**设计值**：后端会自己
    // 按窗口 DPI 换算成非客户区高度，两处不能各乘一次。
    const TitleBarAppearance appearance = titleBarAppearance();
    params.style.captionHeight = appearance.effectiveVisualHeight();
  } else {
    params.style.captionHeight = 0;
  }

  window_ = platform().createWindow(nullptr, params, *this);
  if (window_ == nullptr) return false;

  root_.setDpi(window_->dpi());
  if (background_ != kNoColor) window_->setBackgroundColor(background_);

  // 标题栏 + 内容装进一个纵向容器：自绘标题栏不是「浮在内容上」，
  // 而是真的占掉顶部一行——这样内容区的起点与命中坐标天然一致，
  // 不需要任何偏移补偿。
  auto host = std::make_unique<LinearLayout>(Orientation::kVertical);
  if (options_.customTitleBar) {
    auto* bar = new TitleBar();
    // 标题栏的三个按钮据此操作真实窗口；关窗统一走 closeInterceptor，
    // 让「有未保存内容」只在一个地方判断。
    bar->setWindow(window_);
    bar->setTitle(options_.title);
    bar->setCloseInterceptor([this]() { return onClose_ && onClose_(); });
    bar->setOnDoubleClickBlank([this]() {
      if (window_ != nullptr) window_->toggleMaximize();
    });
    // 左上角的应用图标：与系统标题栏是同一枚（exe 主图标）。
    //
    // 尺寸按 appearance 的设计值乘窗口 DPI —— 图标是位图，按设计值取会让
    // 150% 屏上只有 11 个物理像素再被放大成糊块。取不到就留空，
    // arrangeBand 会连标题的左边距一起省掉。
    const int iconPixels =
        static_cast<int>((static_cast<long long>(bar->appearance().appIconSize) * window_->dpi() +
                          48) /
                         96);
    if (iconPixels > 0) bar->setIcon(platform().applicationIcon(iconPixels));
    titleBar_ = bar;
    host->addView(bar, LayoutParams{}.withSize(LayoutParams::kMatchParent,
                                              LayoutParams::kWrapContent));
  } else {
    titleBar_ = nullptr;
  }

  if (pendingContent_ != nullptr) {
    host->addView(pendingContent_.release(),
                  LayoutParams{}.withSize(LayoutParams::kMatchParent, LayoutParams::kMatchParent)
                      .withWeight(1.0f));
  }

  root_.setTheme(theme_);
  root_.setContentView(std::move(host));

  // 先按真实客户区摆一次，再显示：否则第一帧是空树，会闪一下白。
  const Rect client = window_->clientRect();
  root_.setSize(Size{client.width(), client.height()});
  root_.performLayout();

  if (onDrop_) window_->setFileDropHandler(onDrop_);
  if (frameTimerMs_ > 0) window_->setDelayTimer(frameTimerMs_);

  window_->show(true);
  window_->setFocus();
  return true;
}

int AppWindow::run() {
  if (!create()) return 1;
  closing_ = false;
  // 返回时退出请求已被事件循环消费（见 platform_windows.cpp），
  // 所以这个对象可以再次 run()——例如「关闭后重开」的场景。
  return platform().runEventLoop();
}

void AppWindow::setContentView(std::unique_ptr<ViewGroup> content) {
  if (window_ != nullptr) {
    // 窗口已建：直接从根容器里换成新内容，保留标题栏
    ViewGroup* host = root_.contentView();
    if (host == nullptr) return;
    if (View* previous = host->findView("__pyui_content"))
      host->removeView(previous);
    if (content != nullptr) {
      content->setId("__pyui_content");
      host->addView(content.release(),
                    LayoutParams{}.withSize(LayoutParams::kMatchParent,
                                            LayoutParams::kMatchParent)
                        .withWeight(1.0f));
    }
    root_.performLayout();
    root_.invalidate();
    return;
  }

  pendingContent_ = std::move(content);
  if (pendingContent_ != nullptr) pendingContent_->setId("__pyui_content");
}

// ── 外观 ──

void AppWindow::setTheme(const Theme* theme) {
  theme_ = theme != nullptr ? theme : &defaultTheme();
  root_.setTheme(theme_);
  // 标题栏的配色是构造时算好写进子控件的（见 TitleBar::setTheme），
  // 换肤后必须让它按新主题重算，否则深色主题下会留一条刺白的标题栏。
  if (titleBar_ != nullptr) titleBar_->setTheme(theme_);
  root_.invalidate();
}

void AppWindow::setTitle(std::string title) {
  options_.title = std::move(title);
  if (titleBar_ != nullptr) titleBar_->setTitle(options_.title);
  if (window_ != nullptr) window_->setTitle(options_.title);
}

void AppWindow::setBackgroundColor(Color color) {
  background_ = color;
  if (window_ != nullptr) window_->setBackgroundColor(color);
  root_.invalidate();
}

// ── 行为挂点 ──

void AppWindow::setOnFileDrop(DropHandler fn) {
  onDrop_ = std::move(fn);
  if (window_ != nullptr) window_->setFileDropHandler(onDrop_);
}

void AppWindow::setFrameTimer(int ms) {
  frameTimerMs_ = std::max(0, ms);
  if (window_ == nullptr) return;
  if (frameTimerMs_ > 0) {
    window_->setDelayTimer(frameTimerMs_);
  } else {
    window_->setDelayTimer(0);
  }
}

void AppWindow::refreshAll() {
  root_.invalidate();
  if (window_ != nullptr) window_->invalidate();
}

void AppWindow::quit(int exitCode) {
  closing_ = true;
  platform().quitEventLoop(exitCode);
}

// ── WindowHost ──

void AppWindow::onExpose(const Rect& update) {
  if (window_ == nullptr) return;
  // 画布覆盖本次更新区，Painter 原点已对齐到 update 左上角；
  // ViewRoot::draw 的坐标是客户区坐标，这里不要重复平移。
  std::unique_ptr<Canvas> canvas = window_->createCanvas(update);
  if (!canvas) return;
  root_.draw(canvas->painter(), update);
  canvas->present();
  // 脏区已经真的画掉了，标记清空——否则下一次 consumeDirtyRect
  // 会拿到一块已经被覆盖的区域，做无用功。
  root_.clearDirty();
}

void AppWindow::onResize(int width, int height) {
  root_.setSize(Size{width, height});
  // 尺寸变了标题栏也要重摆（三个按钮贴右 / 标题省略宽度都跟着变）
  if (titleBar_ != nullptr) {
    titleBar_->requestLayout();
    // 最大化状态的最新信号是尺寸变化：AiUI 也是在 onResize 里比较
    // isMaximized() 再通知标题栏。不在这里同步，最大化后按钮图标会一直
    // 停在「最大化」，而它此刻的含义已经是「还原」。
    if (window_ != nullptr) titleBar_->setMaximized(window_->isMaximized());
  }
  root_.invalidate();
}

void AppWindow::onMouseDown(const MouseEvent& e) {
  // viewRoot 恒返回 true（见 view_root.cpp），不能拿它判断是否命中
  root_.dispatchMouseDown(e);
  if (window_ != nullptr) {
    // 按下即捕获：滑块拖出控件外、输入框外拖选文字都靠它。
    // ViewRoot 内部按 pressed_ 转发移动事件，捕获是平台层的对应动作。
    window_->captureMouse(true);
  }
  // 点击可能把焦点移到了输入框（或移出了输入框），闪烁定时器要跟着开关
  syncBlinkTimer();
  syncCursor();
}

void AppWindow::onMouseUp(const MouseEvent& e) {
  root_.dispatchMouseUp(e);
  if (window_ != nullptr) window_->captureMouse(false);
  syncCursor();
}

void AppWindow::onMouseMove(const MouseEvent& e) {
  root_.dispatchMouseMove(e);
  syncCursor();
}

void AppWindow::onMouseLeave() {
  root_.dispatchMouseLeave();
  if (window_ != nullptr) window_->captureMouse(false);
}

void AppWindow::onMouseWheel(const MouseEvent& e) {
  root_.dispatchMouseWheel(e);
}

void AppWindow::onDoubleClick(const MouseEvent& e) {
  // 自绘标题栏的双击由 TitleBar 自己处理（它会把 hitTest 结果标成
  // HTCAPTION，系统通常不会把双击送到这里）。客户区里的双击原样分发。
  root_.dispatchDoubleClick(e);
}

void AppWindow::onContextMenu(const MouseEvent& e) {
  root_.dispatchContextMenu(e);
}

bool AppWindow::onKeyDown(const Key& key, const Modifiers& mods) {
  // 视图树优先：输入框要拿到 Backspace、方向键、Ctrl+A。
  const bool handled = root_.dispatchKeyDown(key, mods);
  // Tab 由 ViewRoot 内置切换焦点，处理完才知道焦点去了哪
  if (key == Key::kTab) syncBlinkTimer();
  if (handled) return true;
  if (onKey_ && onKey_(key, mods)) return true;
  return false;
}

bool AppWindow::onChar(std::uint32_t codepoint) {
  if (root_.dispatchChar(codepoint)) return true;
  if (onChar_ && onChar_(codepoint)) return true;
  return false;
}

void AppWindow::onFocusChanged(bool focused) {
  active_ = focused;
  if (titleBar_ != nullptr) titleBar_->setActive(focused);
  syncBlinkTimer();
}

void AppWindow::syncBlinkTimer() {
  if (window_ == nullptr) return;
  // 只有「窗口激活 + 焦点在输入框」时才需要闪烁。整窗无脑开定时器会让
  // 静态界面每 530ms 被 InvalidateRect 一次，风扇就是这么转起来的。
  bool needsBlink = false;
  if (active_) {
    View* focused = root_.focusedView();
    // 用 cursorShape 判断而不是 className：输入框是唯一要 I 型光标的可聚焦控件，
    // 而 I 型光标恰好也是「这里有插入点」的语义。className 是给错误信息用的。
    needsBlink = focused != nullptr && focused->cursorShape() == CursorShape::kIBeam;
  }
  window_->setBlinkTimer(needsBlink);
}

void AppWindow::onTimer() {
  // 帧定时器与光标闪烁共用同一个 onTimer 入口（后端只有一个回调）。
  // 派发给整棵树是无害的：没动画的控件在 onTimer 里什么都不做。
  root_.dispatchTimer();
  if (window_ != nullptr && frameTimerMs_ > 0) {
    // 单次定时器：回调里要重新排下一次，否则只跑一帧就停了
    window_->setDelayTimer(frameTimerMs_);
  }
}

HitTarget AppWindow::onHitTest(const Point& position) {
  if (titleBar_ == nullptr) return HitTarget::kClient;

  // 命中的控件自己声明了角色就以角色为准——位置和 id 都不参与判断。
  // 这样应用自定义标题栏时，按钮放在哪、用什么 id 都可以，只要写了 role。
  View* hit = root_.hitTest(position);
  if (hit != nullptr) {
    switch (hit->windowRole()) {
      case WindowRole::kMaximize: return HitTarget::kMaxButton;
      case WindowRole::kMinimize: return HitTarget::kClient;
      case WindowRole::kClose: return HitTarget::kClient;
      case WindowRole::kCaption: return HitTarget::kCaption;
      case WindowRole::kNone: break;
    }
  }

  // 标题栏的绝对矩形（窗口客户区坐标）。后端已经先判过缩放带，
  // 所以这里只回答「是不是标题栏」，缩放边不归宿主管。
  const Rect bar = titleBar_->absoluteBounds();
  if (!bar.contains(position)) return HitTarget::kClient;

  // 标题栏空白区：交给系统做拖动、双击最大化、贴边吸附、Snap Layouts。
  // 判到「命中结果在标题栏子树里」也算空白——文字与图标是透明装饰。
  return HitTarget::kCaption;
}

bool AppWindow::onCloseRequest() {
  if (onClose_ && onClose_()) {
    // 宿主拦下了关闭：什么都不做，窗口继续活着
    return true;
  }
  // 不拦就退出事件循环。这里不主动销毁窗口——析构时统一处理，
  // 避免「窗口已销毁但对象还活着」的中间态让后续回调踩空。
  quit(0);
  return true;
}

void AppWindow::onFileDrop(const std::vector<std::string>& paths) {
  if (onDrop_) onDrop_(paths);
}

void AppWindow::syncCursor() {
  if (window_ == nullptr) return;
  // 悬停在哪个控件上，光标就长什么样。ViewRoot 维护唯一的 hot 视图，
  // 所以这里不需要自己跟踪 enter/leave。
  const View* hot = root_.hotView();
  window_->setCursorShape(hot != nullptr ? hot->cursorShape() : CursorShape::kArrow);
}

}  // namespace pyui
