// Window：自绘窗口组件
//
// 把"标题栏 + 内容区"组合成可直接使用的窗口对象：创建平台窗口、
// 摆放两块区域、把非客户区命中交给标题栏、提供模态运行入口。
//
// 用法与 Android 的 Activity 类似：
//   AiUI::Window win;
//   win.setContent(new MyLayout());     // 内容区，Window 接管所有权
//   win.show(owner);                    // 非模态
//   win.showModal(owner);               // 模态
#pragma once

#include "AiUI/platform.h"
#include "AiUI/types.h"
#include "AiUI/view.h"
#include "AiUI/widgets/title_bar.h"

#include <memory>
#include <string>

namespace AiUI {

// 窗口整体配色
struct WindowTheme {
  Color background = kNoColor;
  TitleBarStyle titleBar;
};

class Window {
 public:
  Window();
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  // ── 外观 ──
  void setTitle(const std::wstring& title);
  const std::wstring& title() const { return title_; }
  void setTheme(const WindowTheme& theme);
  void setStyle(const WindowStyle& style);
  // 最小尺寸（96 DPI 设计值）
  void setMinSize(int width, int height);

  // 标题栏可直接定制；不需要标题栏时用 setTitleBarVisible(false)。
  // 高度等细节改 titleBar()->setHeight(...)，不要另开入口，否则会和
  // WindowStyle::captionHeight 打架。
  TitleBar* titleBar() const { return titleBar_; }
  void setTitleBarVisible(bool visible);
  bool isTitleBarVisible() const { return titleBarVisible_; }

  // ── 内容 ──
  // 接管所有权；再次调用会销毁旧内容
  void setContentView(View* content);
  View* contentView() const { return content_; }

  // ── 生命周期 ──
  // 创建窗口并挂到 owner 下（顶层窗口时 owner 可为空）
  bool create(NativeHandle owner);
  // 创建后设置初始外框位置与客户区尺寸（在 show 之前调用）
  void setInitialBounds(const Rect& bounds);
  void show(bool visible = true);
  void close();
  bool isOpen() const { return window_ != nullptr; }

  // 模态运行：禁用 owner，直到本窗口关闭。返回是否被用户确认关闭
  void showModal(NativeHandle owner);

  NativeHandle handle() const;
  Rect clientRect() const;
  int dpi() const;

  // Esc 是否关闭窗口；对话框用 true，主窗口保持 false
  void setEscapeCloses(bool closes) { escapeCloses_ = closes; }

  // 关闭请求回调；返回 true 表示已自行处理，不再真正关闭
  using CloseHandler = std::function<bool()>;
  void setOnClose(CloseHandler fn) { onClose_ = std::move(fn); }

  // 内容变化后重新排布
  void requestLayout();

 private:
  // 内部根容器：纵向排列标题栏与内容
  class Root;

  void applyTitleBarButtons();
  void syncMaximizedState();

  std::unique_ptr<Root> root_;
  View* content_ = nullptr;
  TitleBar* titleBar_ = nullptr;
  PlatformWindow* window_ = nullptr;

  std::wstring title_;
  WindowTheme theme_;
  WindowStyle style_;
  int minWidth_ = 0;
  int minHeight_ = 0;
  bool titleBarVisible_ = true;
  bool escapeCloses_ = false;

  CloseHandler onClose_;
};

}  // namespace AiUI
