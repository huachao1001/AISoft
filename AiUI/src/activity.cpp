// Activity 实现：把 XML 布局、Window 与生命周期串起来
#include "AiUI/activity.h"

#include "AiUI/layout_inflater.h"

namespace AiUI {

Activity::Activity() = default;

Activity::~Activity() {
  // 窗口对象析构会连带销毁整棵视图树
  window_.reset();
}

void Activity::setTitle(const std::wstring& title) {
  title_ = title;
  if (window_) window_->setTitle(title);
}

void Activity::setStyle(const WindowStyle& style) {
  style_ = style;
  if (window_) window_->setStyle(style);
}

void Activity::setMinSize(int width, int height) {
  minWidth_ = width;
  minHeight_ = height;
  if (window_) window_->setMinSize(width, height);
}

void Activity::setEscapeCloses(bool closes) {
  escapeCloses_ = closes;
  if (window_) window_->setEscapeCloses(closes);
}

void Activity::buildContent() {
  // XML 优先；没给就用子类自建的视图
  View* content = nullptr;
  if (const wchar_t* name = layoutName()) {
    content = LayoutInflater::inflate(name);
  }
  if (!content) content = createView();
  if (content) window_->setContentView(content);
}

bool Activity::create(NativeHandle owner) {
  if (created_) return isOpen();

  window_ = std::make_unique<Window>();

  // Window 目前只建顶层窗口；标题栏与边框由它自绘
  WindowStyle style = style_;
  window_->setStyle(style);
  window_->setTitle(title_);
  window_->setEscapeCloses(escapeCloses_);
  if (minWidth_ > 0 || minHeight_ > 0) window_->setMinSize(minWidth_, minHeight_);

  buildContent();

  // 关闭请求先问子类，再交给平台销毁。
  // 返回 true 表示放行，此时是最后一次能安全访问控件的机会。
  window_->setOnClose([this]() {
    if (!onCloseRequest()) return false;
    if (created_) {
      created_ = false;
      onDestroy();
    }
    return true;
  });

  // 内容区没消费的按键转给子类
  window_->setOnKeyDown([this](const Key& key, const Modifiers& mods) {
    return onKeyDown(key, mods);
  });

  if (!window_->create(owner)) {
    window_.reset();
    return false;
  }

  created_ = true;
  // 此时布局已挂上窗口，id 已生效，findView 可用
  onCreate();
  return true;
}

void Activity::setInitialBounds(const Rect& bounds) {
  if (window_) window_->setInitialBounds(bounds);
}

void Activity::show(bool visible) {
  if (window_) window_->show(visible);
}

void Activity::showModal(NativeHandle owner) {
  if (!create(owner)) return;
  window_->showModal(owner);
  // 用户可能直接销毁窗口而不走关闭回调，这里补一次兜底
  if (created_) {
    created_ = false;
    onDestroy();
  }
}

void Activity::close() {
  if (window_) window_->close();
}

bool Activity::isOpen() const { return window_ && window_->isOpen(); }

View* Activity::contentView() const { return window_ ? window_->contentView() : nullptr; }

View* Activity::findView(const char* id) const {
  View* content = contentView();
  if (!content || !id) return nullptr;
  return content->findView(id);
}

int Activity::dpi() const { return window_ ? window_->dpi() : 96; }

}  // namespace AiUI
