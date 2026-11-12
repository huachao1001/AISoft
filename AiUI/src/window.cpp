// Window 实现：标题栏 + 内容区的组合窗口
#include "AiUI/window.h"

#include "AiUI/font.h"
#include "AiUI/layouts/linear_layout.h"
#include "AiUI/theme.h"
#include "AiUI/view_root.h"

#include <algorithm>

namespace AiUI {
namespace {

// 顶层窗口类名在进程内唯一即可；不同 Window 实例共用
constexpr const wchar_t* kTopLevelClass = L"AiUIWindow";

}  // namespace

// 内部根容器：纵向放标题栏与内容
class Window::Root : public LinearLayout {
 public:
  Root() : LinearLayout(Orientation::kVertical) {}
  ~Root() override = default;

  void setTitleBar(TitleBar* bar) { titleBar_ = bar; }

 protected:
  void arrangeChildren(const Rect& content) override {
    // 标题栏高度是动态的（可自定义），每轮布局都同步一次。
    // 布局参数按 96 DPI 设计值传，缩放由 View::measure 统一做，这里不能再乘。
    if (titleBar_ && titleBar_->isVisible()) {
      const int height = titleBar_->height();
      LayoutParams lp = titleBar_->layoutParams();
      if (lp.height != height) {
        lp.height = height;
        titleBar_->setLayoutParams(lp);
      }
    }
    LinearLayout::arrangeChildren(content);
  }

 private:
  TitleBar* titleBar_ = nullptr;
};

Window::Window() : root_(std::make_unique<Root>()) {
  root_->setBackgroundColor(defaultTheme().body);

  auto* bar = new TitleBar();
  titleBar_ = bar;
  root_->setTitleBar(bar);
  bar->setAppIcon(platform()->appIcon());
  root_->addView(bar, LayoutParams{}.withSize(LayoutParams::kMatchParent, bar->height()));

  // 初始主题落在整棵树上：标题栏和内容都从根节点继承
  appTheme_ = &kLightTheme;
  root_->setTheme(appTheme_);
  bar->setTheme(appTheme_);

  // 标题栏按钮转成窗口动作
  bar->setOnMinimize([this] {
    if (window_) window_->minimize();
  });
  bar->setOnToggleMaximize([this] { syncMaximizedState(); });
  bar->setOnClose([this] {
    if (onClose_ && !onClose_()) return;
    if (window_) window_->close();
  });
}

Window::~Window() {
  // 先断开平台窗口，再销毁视图树
  if (window_) {
    window_->setBlinkTimer(false);
    platform()->destroyWindow(window_);
    window_ = nullptr;
  }
  content_ = nullptr;
  titleBar_ = nullptr;
  // root_ 析构时会删掉标题栏与内容
}

// ───────────────────────── 外观 ─────────────────────────

void Window::setTitle(const std::wstring& title) {
  title_ = title;
  if (titleBar_) titleBar_->setTitle(title);
  if (window_) window_->setTitle(title);
}

void Window::setTheme(const WindowTheme& theme) {
  theme_ = theme;
  if (titleBar_) titleBar_->setStyle(theme_.titleBar);
  if (root_) {
    const Color fallback = appTheme_ ? appTheme_->body : defaultTheme().body;
    root_->setBackgroundColor(theme_.background == kNoColor ? fallback : theme_.background);
  }
  if (window_) window_->invalidate();
}

void Window::setAppTheme(const Theme* theme) {
  appTheme_ = theme;
  if (root_) root_->setTheme(theme);
  if (titleBar_) {
    // 标题栏从自身主题取色，先挂上再重算样式
    titleBar_->setTheme(theme);
    titleBar_->setStyle(theme_.titleBar);
  }
  if (root_) {
    const Color bg = theme_.background != kNoColor
                         ? theme_.background
                         : (theme ? theme->body : defaultTheme().body);
    root_->setBackgroundColor(bg);
  }
  if (window_) window_->invalidate();
}

void Window::setDarkMode(bool dark) {
  if (darkMode_ == dark) return;
  darkMode_ = dark;
  setAppTheme(dark ? &kDarkTheme : &kLightTheme);
}

void Window::setStyle(const WindowStyle& style) {
  style_ = style;
  applyTitleBarButtons();
  // 视觉总高由窗口样式给；0 表示回到逻辑高度
  if (titleBar_) titleBar_->setHeight(style.captionHeight);
  requestLayout();
}

void Window::setMinSize(int width, int height) {
  minWidth_ = width;
  minHeight_ = height;
}

void Window::setTitleBarVisible(bool visible) {
  if (titleBarVisible_ == visible) return;
  titleBarVisible_ = visible;
  if (titleBar_) titleBar_->show(visible);
  requestLayout();
}

// ───────────────────────── 内容 ─────────────────────────

void Window::setContentView(View* content) {
  if (content_ == content || !root_) return;

  if (content_ && content_->parent() == root_.get()) {
    // removeView 会连带销毁，旧内容不再保留
    root_->removeView(content_);
  }
  content_ = content;
  if (!content_) return;

  root_->addView(content_, LayoutParams{}.withWeight(1.0f).withSize(LayoutParams::kMatchParent,
                                                                    LayoutParams::kMatchParent));
  requestLayout();
}

void Window::requestLayout() {
  if (root_ && root_->root()) root_->root()->requestLayout();
}

// ───────────────────────── 生命周期 ─────────────────────────

bool Window::create(NativeHandle owner) {
  if (window_) return true;

  WindowParams params;
  params.className = kTopLevelClass;
  params.visible = false;
  params.tabStop = true;
  params.title = title_;
  params.minWidth = minWidth_;
  params.minHeight = minHeight_;

  // 标题栏由 AiUI 自绘，系统那条让给客户区；
  // 标题栏高度由 Root 每轮布局按 titleBar_->height() 同步。
  params.style = style_;
  params.style.topLevel = true;

  if (!root_->create(owner, params, Rect{})) return false;
  ViewRoot* viewRoot = root_->root();
  window_ = viewRoot->platformWindow();
  if (!window_) return false;

  applyTitleBarButtons();
  if (titleBar_) titleBar_->setTitle(title_);
  if (theme_.background != kNoColor) root_->setBackgroundColor(theme_.background);

  // 关闭走视图树回调，方便内容拦截（如未保存确认）
  viewRoot->setCloseHandler([this]() { return onClose_ ? onClose_() : true; });
  // 内容区没消费的按键交给宿主做窗口级快捷键
  viewRoot->setKeyHandler([this](const Key& key, const Modifiers& mods) {
    return onKey_ ? onKey_(key, mods) : false;
  });
  // 对话框用 Esc 关闭；主窗口不响应 Esc
  viewRoot->setEscapeCloses(escapeCloses_);
  // 最大化状态回来后更新标题栏图标
  viewRoot->setMaximizeStateHandler([this](bool maximized) {
    if (titleBar_) titleBar_->setMaximized(maximized);
  });
  return true;
}

void Window::show(bool visible) {
  if (window_) window_->show(visible);
}

void Window::setInitialBounds(const Rect& bounds) {
  if (!window_) return;
  // 平台层按客户区解释 bounds；先定尺寸再定位，避免先移动后跳一下
  window_->setBounds(bounds);
}

void Window::close() {
  // 只发关闭请求：真正的销毁由平台窗口走 WM_CLOSE 与 ViewRoot 回调
  if (!window_) return;
  window_->close();
}

void Window::showModal(NativeHandle owner) {
  if (!create(owner)) return;
  show(true);
  if (!window_) return;
  window_->setFocus();
  runModalLoop(window_->handle(), owner);
  // 用户可能点关闭按钮（窗口已销毁），也可能调用方自己收尾；
  // 这里主动回收平台窗口，避免对象留在后端表里
  if (window_) {
    window_->setBlinkTimer(false);
    platform()->destroyWindow(window_);
    window_ = nullptr;
  }
}

NativeHandle Window::handle() const { return window_ ? window_->handle() : nullptr; }

Rect Window::clientRect() const { return window_ ? window_->clientRect() : Rect{}; }

int Window::dpi() const { return window_ ? window_->dpi() : platform()->screenDpi(); }

// ───────────────────────── 内部 ─────────────────────────

void Window::applyTitleBarButtons() {
  if (!titleBar_) return;
  titleBar_->setMinimizable(style_.topLevel && style_.minimizable);
  titleBar_->setMaximizable(style_.topLevel && style_.maximizable);
}

void Window::syncMaximizedState() {
  if (!window_ || !titleBar_) return;
  // 最大化是异步的，状态由 onResize 回来后再刷新，这里只发命令
  window_->toggleMaximize();
}

}  // namespace AiUI
