// TitleBar：自绘标题栏
//
// 完全由通用控件拼装，不依赖任何平台绘制代码：
//
//   TitleBar (FrameLayout)
//   └── band : LinearLayout(水平)      贴顶，高度即"逻辑高度"
//       ├── ImageView     应用图标
//       ├── TextView      标题
//       ├── View          撑开左右两端的占位
//       └── LinearLayout  三个系统按钮
//
// 逻辑高度决定系统按钮的悬停底色高度、图标与标题的垂直居中基准，也是可拖动
// 区域。视觉总高可以更大，多出来的部分交给使用者自绘，超出即不显示。
//
// 所有部件都带 id，宿主用 findViewById(kTitleBarIdXxx) 拿到后即可改样式、
// 换配色、加子视图，不需要继承 TitleBar。
#pragma once

#include "AiUI/layouts/frame_layout.h"
#include "AiUI/platform.h"

#include <functional>
#include <string>

namespace AiUI {

// 只用指针，不引入控件头文件：宿主工程往往先包含 <windows.h>，
// 其中 small / near 之类的宏会和控件里的标识符打架。
class Button;
class ImageView;
class LinearLayout;
class TextView;

// 标题栏上的系统按钮
enum class TitleBarButton {
  kNone,
  kMinimize,
  kMaximize,
  kClose,
};

// 标题栏内部部件的 id；宿主据此定制外观
constexpr int kTitleBarIdRoot = 0x7A00;
constexpr int kTitleBarIdBand = 0x7A01;
constexpr int kTitleBarIdIcon = 0x7A02;
constexpr int kTitleBarIdTitle = 0x7A03;
constexpr int kTitleBarIdButtons = 0x7A04;
constexpr int kTitleBarIdMinimize = 0x7A05;
constexpr int kTitleBarIdMaximize = 0x7A06;
constexpr int kTitleBarIdClose = 0x7A07;

// 标题栏默认度量（96 DPI 设计值）。平台差异只有按钮在哪一侧与几个数字，
// 实现放在 src/os/<platform>/，通用层只读数据。
struct TitleBarDefaults {
  // 逻辑高度：按钮悬停底色高度、图标与标题垂直居中的基准、可拖动区域
  int bandHeight = 38;
  // 系统按钮尺寸；宽度是每个按钮的格宽
  int buttonWidth = 46;
  int iconSize = 11;
  // 标题左侧应用图标尺寸；0 表示不显示
  int appIconSize = 18;
  int padLeft = 12;
  // 按钮一侧的内边距；0 让悬停底色贴到窗口边沿
  int padRight = 0;
  int iconGap = 8;
  // 系统按钮在左侧（macOS）还是右侧（Windows / 多数 Linux）
  bool buttonsOnLeft = false;
};

// 当前平台的默认度量
const TitleBarDefaults& titleBarDefaults();

// 标题栏配色
struct TitleBarStyle {
  Color background = makeColor(0xff, 0xff, 0xff);
  Color activeBackground = makeColor(0xff, 0xff, 0xff);
  Color text = makeColor(0x1a, 0x1a, 0x1a);
  Color inactiveText = makeColor(0x99, 0x99, 0x99);
  // 系统按钮：悬停底、按下底、关闭按钮的红色底与白色图标
  Color buttonHover = makeColor(0xe8, 0xe8, 0xe8);
  Color buttonDown = makeColor(0xd8, 0xd8, 0xd8);
  Color closeHover = makeColor(0xe8, 0x11, 0x23);
  Color closeDown = makeColor(0xc5, 0x0e, 0x1d);
  Color closeInk = makeColor(0xff, 0xff, 0xff);
  Color line = makeColor(0xe5, 0xe5, 0xe5);
  // 底部分隔线高度；0 表示不画
  int lineThickness = 1;
};

class TitleBar : public FrameLayout { public:
  using Handler = std::function<void()>;

  TitleBar();
  ~TitleBar() override;

  // ── 外观 ──
  void setStyle(const TitleBarStyle& style);
  const TitleBarStyle& style() const { return style_; }

  // 视觉总高（96 DPI 设计值）；0 表示等于逻辑高度
  void setHeight(int height);
  int height() const;

  // 逻辑高度（96 DPI 设计值）：按钮悬停底色高度与垂直居中基准。
  // 改它等于改 band 的高度。
  void setBandHeight(int height);
  int bandHeight() const;

  void setTitle(const std::wstring& title);
  const std::wstring& title() const { return title_; }
  // 标题左侧的应用图标；无效时不画
  void setAppIcon(Image icon);
  void setTitleIconVisible(bool visible);
  bool isTitleIconVisible() const { return showTitleIcon_; }

  // ── 按钮 ──
  void setMinimizable(bool v);
  void setMaximizable(bool v);
  void setMaximized(bool v);
  bool isMaximized() const { return maximized_; }
  // 窗口失去/重新获得激活时标题与图标变灰
  void setActive(bool v);

  void setOnMinimize(Handler fn) { onMinimize_ = std::move(fn); }
  void setOnToggleMaximize(Handler fn) { onToggleMaximize_ = std::move(fn); }
  void setOnClose(Handler fn) { onClose_ = std::move(fn); }

  // 内部部件；等价于 findViewById(kTitleBarIdXxx)
  LinearLayout* band() const { return band_; }
  Button* systemButton(TitleBarButton button) const;

  HitTarget onHitTest(const Point& windowPoint) override;

 protected:
  Size onMeasure(const Size& available) override;
  void onPaint(Painter& painter, const Rect& rect) override;
  Color backgroundColor() const override { return style_.background; }

 private:
  void applyStyle();
  void syncButtonIcons();

  TitleBarStyle style_;
  TitleBarDefaults defaults_;
  std::wstring title_;
  Image appIcon_;
  bool showTitleIcon_ = true;
  // 0 表示用默认逻辑高度
  int bandHeightOverride_ = 0;
  // 0 表示视觉总高等于逻辑高度
  int heightOverride_ = 0;
  bool minimizable_ = true;
  bool maximizable_ = true;
  bool maximized_ = false;
  bool active_ = true;

  LinearLayout* band_ = nullptr;
  ImageView* iconView_ = nullptr;
  TextView* titleView_ = nullptr;
  LinearLayout* buttons_ = nullptr;
  Button* minimizeButton_ = nullptr;
  Button* maximizeButton_ = nullptr;
  Button* closeButton_ = nullptr;

  Handler onMinimize_;
  Handler onToggleMaximize_;
  Handler onClose_;
};

}  // namespace AiUI
