// TitleBar：自绘标题栏
//
// ── 结构由 XML 定义 ──
//
// 标题栏的内容不是硬编码在构造函数里的，而是一份内置布局
// `assets/layouts/titlebar.xml`，结构与 AiUI 的 TitleBar 逐节点对齐：
//
//   TitleBar (FrameLayout)
//   └── band : LinearLayout(水平)
//       ├── ImageView    应用图标    0x7A02
//       ├── TextView     标题        0x7A03
//       ├── View         撑开占位    0x7A08
//       └── LinearLayout 三个按钮    0x7A04（0x7A05..07）
//
// 应用在自己的 assets/layouts/ 里放同名文件即覆盖默认实现，不需要改 C++。
// 这条路径走的是和普通布局完全相同的 LayoutInflater，所以「能改标题栏」
// 和「能写一个布局」是同一件能力，不需要另学一套。
//
// ── 为什么数字不写进 XML ──
//
// XML 只描述**结构**：谁在谁里面、顺序、谁撑开。高度、内边距、配色、字号、
// 图标尺寸全部来自 TitleBarAppearance（平台度量 + 主题）。把 38/46/11 写进
// XML 意味着「换一个平台就换一份 XML」，而那正是标题栏最不需要的分叉：
// 三个平台的标题栏差异只是几组数字，共用一份结构才能保证它们不会各自漂移。
//
// 换成新结构之后，TitleBar 仍然负责「按 appearance 精确摆放」：band 内部
// 的间距与按钮尺寸是运行期算的，因为 macOS 的圆点按钮是正方形、只在左
// 侧且有序号翻转，这些都不是静态布局能表达的。两层的分工是：
//
//   布局层  —— 有哪几个部件、谁跟着谁、谁负责撑开
//   控件层  —— 这些部件各自多大、离边缘多远、悬停什么颜色
//
// ── 平台无关 ──
//
// 这里没有一行平台代码（铁律五）。所有平台差异都来自 TitleBarAppearance。
#pragma once

#include "pyui/platform/title_bar.h"
#include "pyui/view_group.h"
#include "pyui/widgets/button.h"
#include "pyui/widgets/image_view.h"
#include "pyui/widgets/text_view.h"

#include <functional>
#include <string>
#include <vector>

namespace pyui {

class PlatformWindow;

class TitleBar : public ViewGroup {
 public:
  // 内置默认布局的名字。应用覆盖时用同一个名字即可。
  static constexpr const char* kDefaultLayout = "titlebar";

  TitleBar();
  ~TitleBar() override;

  // 平台外观；构造时取 titleBarAppearance()，也可显式覆盖（截图、测试）
  void setAppearance(const TitleBarAppearance& appearance);
  const TitleBarAppearance& appearance() const { return appearance_; }

  // 绑定的窗口：三个按钮据此操作真实窗口。为空时按钮只发回调，
  // 用于离线渲染与测试。
  void setWindow(PlatformWindow* window);
  PlatformWindow* window() const { return window_; }

  void setTitle(std::string title);
  const std::string& title() const;

  // 窗口是否激活：非激活时标题与底色变淡，与系统标题栏一致
  void setActive(bool active);
  bool isActive() const { return active_; }

  // 最大化状态：按钮图标在「最大化 / 还原」之间切换。
  // AiUI 在 ViewRoot::onResize 里比较 isMaximized() 的变化再通知标题栏，
  // PyUI 同一条路径（AppWindow::onResize）。做成幂等：状态没变就什么都不做，
  // 否则每次窗口尺寸变化都会重设图标，白白触发一次重排。
  void setMaximized(bool maximized);
  bool isMaximized() const { return maximized_; }

  // 应用图标（可选）；无效 Image 时按 appearance.iconSize 留白
  void setIcon(const Image& image);
  void setIconResourceId(int resourceId);

  // 双击空白区：默认切换最大化
  void setOnDoubleClickBlank(std::function<void()> fn) { onDoubleClickBlank_ = std::move(fn); }
  // 关闭前的拦截（如有未保存内容）；返回 true 表示阻止关闭
  using CloseInterceptor = std::function<bool()>;
  void setCloseInterceptor(CloseInterceptor fn) { onCloseRequest_ = std::move(fn); }

  // ── 部件 id：沿用 AiUI 的 0x7A00–0x7A08 ──
  //
  // 为什么用数值而不是哈希：这是**跨库契约**。同一份 titlebar.xml 在 AiUI
  // 与 PyUI 里都该指到同一个部件，业务代码换库时不该改 id。
  static constexpr int kIdRoot = 0x7A00;
  static constexpr int kIdBand = 0x7A01;
  static constexpr int kIdIcon = 0x7A02;
  static constexpr int kIdTitle = 0x7A03;
  static constexpr int kIdButtons = 0x7A04;
  static constexpr int kIdMinimize = 0x7A05;
  static constexpr int kIdMaximize = 0x7A06;
  static constexpr int kIdClose = 0x7A07;
  static constexpr int kIdSpacer = 0x7A08;

  // 字符串形式的同一批 id；findView("titleBar.close") 仍然可用
  static constexpr const char* kIdBandName = "titleBar.band";
  static constexpr const char* kIdIconName = "titleBar.icon";
  static constexpr const char* kIdTitleName = "titleBar.title";
  static constexpr const char* kIdMinimizeName = "titleBar.minimize";
  static constexpr const char* kIdMaximizeName = "titleBar.maximize";
  static constexpr const char* kIdCloseName = "titleBar.close";

  const char* className() const override { return "TitleBar"; }

  // 换肤要重取缓存色：标题色、按钮配色都是构造/设外观时算好写进子控件的，
  // 不像纯绘制控件那样在 onPaint 里现取。
  void setTheme(const Theme* theme) override;

 protected:
  void onPaint(Painter& painter, const Rect& rect) override;
  Size onMeasure(const Size& available) override;
  void onLayout(const Rect& frame) override;
  bool onMouseDown(const MouseEvent& e) override;
  void onDoubleClick(const MouseEvent& e) override;
  CursorShape cursorShape() const override { return CursorShape::kArrow; }

  // 命中测试：落在 band 空白处时回落到 TitleBar 自己。
  //
  // 为什么不靠 mouseTransparent：band 是按钮的祖先，把它标成透明会连按钮
  // 一起跳过。而 band 铺满整条标题栏，如果命中停在它上面，拖动窗口与
  // 双击最大化就收不到事件——那是标题栏最核心的两个交互。
  View* hitTest(Point localPoint) const override;

 private:
  // 从布局树里取回各部件句柄；任一必须部件缺失时返回 false
  bool bindParts();
  // 按窗口角色在子树里找控件。角色来自 XML 的 role 属性，与 id、位置无关，
  // 所以应用自定义标题栏时按钮换个 id、挪到别处也照样能接上。
  View* findByRole(WindowRole role);
  template <typename T>
  T* findByRoleAs(WindowRole role) {
    return dynamic_cast<T*>(findByRole(role));
  }
  // 按 appearance 摆 band 内的图标、标题与按钮条
  void arrangeBand(const Rect& frame);
  void updateButtonStates();
  void updatePalette();

  TitleBarAppearance appearance_;
  PlatformWindow* window_ = nullptr;
  bool active_ = true;
  bool maximized_ = false;
  Image icon_;

  // 部件句柄。布局来自 XML，所以这些指针在构造后一次性解析出来，
  // 而不是像以前那样边 new 边保存——覆盖布局换了层级也不影响使用。
  ViewGroup* band_ = nullptr;
  ImageView* iconView_ = nullptr;
  TextView* titleView_ = nullptr;
  ViewGroup* buttonStrip_ = nullptr;
  Button* minimize_ = nullptr;
  Button* maximize_ = nullptr;
  Button* close_ = nullptr;

  CloseInterceptor onCloseRequest_;
  std::function<void()> onDoubleClickBlank_;
};

}  // namespace pyui
