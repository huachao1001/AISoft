// AppWindow：把 ViewRoot 接到平台窗口上的宿主胶水层
//
// 为什么库需要这一层：
//   平台后端只做两件事——把原生消息翻译成中性事件、提供一块画布。它不知道
//   视图树的存在。ViewRoot 反过来只管布局与分发，不知道窗口的存在。两者之间
//   的连线（建画布、转发脏区、标题栏命中、定时器、关闭拦截、拖放）全是样板，
//   留给每个应用自己写，必然会写出四份略有差别的版本，其中至少一份会漏掉
//   WM_ACTIVATE 或拖放——而这类漏掉的表现是「界面看着对，某个操作就是不响应」，
//   排查成本远高于一条接线。
//
// 所以这里实现 WindowHost 的全部回调，原样转给 ViewRoot，并把自绘标题栏
// 接上真实窗口。应用只剩下三行：
//
//   pyui::enableHighDpi();
//   pyui::AppWindow app({.title = "计算器", ...});
//   app.setContentView(pyui::LayoutInflater::inflate("calc"));
//   return app.run();
//
// 这一层不隐藏任何能力：需要自定义键处理、关窗提示、拖放、动画时钟时，
// 挂回调即可；需要直接操作窗口时 getWindow() 就是 PlatformWindow*。
#pragma once

#include "pyui/layouts/linear_layout.h"
#include "pyui/platform/platform.h"
#include "pyui/platform/window.h"
#include "pyui/view_root.h"
#include "pyui/widgets/title_bar.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pyui {

class AppWindow final : public WindowHost {
 public:
  struct Options {
    std::string title = "PyUI";
    // 窗口初始尺寸与最小尺寸，一律是 96 DPI 设计值：高 DPI 屏上
    // AppWindow 自己按屏幕 DPI 换算成物理像素，调用方不用管。
    int width = 760;
    int height = 520;
    int minWidth = 320;
    int minHeight = 240;

    bool resizable = true;
    bool minimizable = true;
    bool maximizable = true;

    // 标题栏。有三种形态，由这个开关决定：
    //
    //   true（默认） 用内置的自绘标题栏（TitleBar，从 titlebar.xml 装配）。
    //   false        不画任何标题栏，内容铺满整个窗口。窗口边框、阴影、贴边
    //                吸附仍由系统提供，但顶部没有拖动条——需要拖动的话，
    //                在自己的布局里放一个 role="caption" 的 View。
    //
    // 注意这**不是**「换成 Windows 系统标题栏」：PyUI 的窗口始终是自绘的，
    // 系统标题栏在任何配置下都不会出现。要做无边框外观就关掉它，
    // 然后在自己的布局里声明 role，拖动与三个按钮依旧可用。
    bool customTitleBar = true;

    // 本可执行文件里的图标资源 id；0 表示由平台决定（通常取 exe 主图标）
    int iconResourceId = 0;
    // 窗口类名，需在进程内唯一；空则由后端生成
    std::string className;
  };

  explicit AppWindow(Options options = {});
  ~AppWindow() override;

  AppWindow(const AppWindow&) = delete;
  AppWindow& operator=(const AppWindow&) = delete;

  // ── 启动 ──
  // 建窗口并挂上内容。run() 会隐式调用它；分两步是为了「先 create，
  // 再按真实客户区尺寸补一次布局」这类需要窗口已存在的场景。
  bool isCreated() const { return window_ != nullptr; }
  bool create();
  // 显示窗口并进入事件循环，返回退出码。
  int run();

  // ── 内容 ──
  // 必须在 create() 之前调用（之后调用请走 root().setContentView）。
  void setContentView(std::unique_ptr<ViewGroup> content);
  ViewRoot& root() { return root_; }
  const ViewRoot& root() const { return root_; }

  PlatformWindow* getWindow() const { return window_; }
  // 自绘标题栏；customTitleBar = false 时为 nullptr
  TitleBar* titleBar() const { return titleBar_; }

  // ── 外观与标题 ──
  void setTheme(const Theme* theme);
  const Theme* theme() const { return theme_; }
  void setTitle(std::string title);
  const std::string& title() const { return options_.title; }
  void setBackgroundColor(Color color);

  // ── 行为挂点 ──
  // 关窗拦截：返回 true 表示「先别关」（例如有未保存内容）。
  // 不设时关闭即退出事件循环。Alt+F4、系统菜单、标题栏关闭按钮走同一条路，
  // 所以在这里判断一次就覆盖全部入口。
  using CloseHandler = std::function<bool()>;
  void setOnClose(CloseHandler fn) { onClose_ = std::move(fn); }

  // 视图树没消费的按键在这里兜底，用于全局快捷键（Esc、Ctrl+S……）。
  using KeyHandler = std::function<bool(const Key&, const Modifiers&)>;
  void setKeyHandler(KeyHandler fn) { onKey_ = std::move(fn); }

  // 视图树没消费的可打印字符在这里兜底。
  //
  // 为什么需要：字符输入默认只送给焦点控件（输入框）。像计算器这种「整窗
  // 都吃数字键」的界面没有输入框可聚焦，字符会全部丢掉——键盘按 1+2 毫无
  // 反应，而鼠标点按钮却正常，这个不一致很难自己想到。
  using CharHandler = std::function<bool(std::uint32_t)>;
  void setCharHandler(CharHandler fn) { onChar_ = std::move(fn); }

  // 拖入文件（绝对路径，UTF-8）。设了回调才会打开窗口的拖放接收，
  // 没设时不接收——接收了又悄悄丢掉会让拖放源显示错误的光标。
  using DropHandler = std::function<void(const std::vector<std::string>&)>;
  void setOnFileDrop(DropHandler fn);

  // 动画时钟：每 ms 毫秒派发一次视图树计时器（ProgressBar 的跑马灯、
  // 倒计时这类需要持续推进的控件靠它）。0 表示关闭。
  //
  // 默认关闭：只在真正有动画时才让整棵树按帧重绘，静态界面不该每秒
  // 白画 60 次——这也是「脏区优先」的一部分。
  void setFrameTimer(int ms);

  // 强制整窗重绘（换主题、换布局后立即生效）
  void refreshAll();

  // ── WindowHost ──
  void onExpose(const Rect& update) override;
  void onResize(int width, int height) override;
  void onMouseDown(const MouseEvent& e) override;
  void onMouseUp(const MouseEvent& e) override;
  void onMouseMove(const MouseEvent& e) override;
  void onMouseLeave() override;
  void onMouseWheel(const MouseEvent& e) override;
  void onDoubleClick(const MouseEvent& e) override;
  void onContextMenu(const MouseEvent& e) override;
  bool onKeyDown(const Key& key, const Modifiers& mods) override;
  bool onChar(std::uint32_t codepoint) override;
  void onFocusChanged(bool focused) override;
  void onTimer() override;
  HitTarget onHitTest(const Point& position) override;
  bool onCloseRequest() override;
  void onFileDrop(const std::vector<std::string>& paths) override;

 private:
  // 初始窗口矩形：按设计值换算后在工作区居中
  Rect centeredBounds(int dpi) const;
  // 更新悬停控件的鼠标形状
  void syncCursor();
  // 光标闪烁只在焦点落在输入框时跑：整窗每 530ms 重画一次纯属浪费
  void syncBlinkTimer();
  // 退出事件循环；窗口在析构时统一销毁
  void quit(int exitCode);
  // 把布局里 role="minimize/maximize/close" 的控件接到真实窗口动作上。
  //
  // 只补「没挂过 onClick」的那些：内置标题栏已经在构造函数里接好了，
  // 应用自己写的 onClick 也不能被覆盖。这样「写 role 就有行为」与
  // 「内置标题栏照旧」两条同时成立。
  void bindWindowRoles(ViewGroup* root);

  Options options_;
  ViewRoot root_;
  PlatformWindow* window_ = nullptr;
  TitleBar* titleBar_ = nullptr;
  // create() 之前暂存内容；create() 时并入带标题栏的根容器
  std::unique_ptr<ViewGroup> pendingContent_;
  const Theme* theme_ = &defaultTheme();
  int frameTimerMs_ = 0;
  Color background_ = kNoColor;
  bool closing_ = false;
  bool active_ = true;

  CloseHandler onClose_;
  KeyHandler onKey_;
  CharHandler onChar_;
  DropHandler onDrop_;
};

}  // namespace pyui
