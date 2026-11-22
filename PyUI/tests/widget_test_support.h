// 控件测试的公共基础设施
//
// 三个控件测试文件都会重复「摆好尺寸 → 画一遍 → 比对」这套动作，收在这里，
// 免得每个文件各写一份不一致的脚手架。这里只放不涉及具体控件的部分：
//
//   recordDirect()  录绘制命令（跨平台逐字节比对的那一层）
//   renderDirect()  用 SoftwareBackend + MockTextRenderer 出像素（不变量那层）
//   RootHarness     需要悬停/焦点/按下态时才用：这些状态只由 ViewRoot 维护，
//                   测试不能直接改控件里的标志位
//
// 关于 MockTextRenderer：字体缓存是进程单例，换渲染器会销毁旧字体句柄。
// 所以这里只创建一个、永不释放——每个测试各建一个会拿到悬空句柄。
#pragma once

#include "test_framework.h"

#include "pyui/layouts/frame_layout.h"
#include "pyui/software_backend.h"
#include "pyui/testing/recording_painter.h"
#include "pyui/text_renderer.h"
#include "pyui/theme.h"
#include "pyui/view_root.h"

#include <memory>
#include <string>
#include <vector>

namespace pyui {
namespace test {

// ── 环境 ──

// 测试进程共用的 Mock 字体渲染器。首次调用时装进 FontCache。
inline MockTextRenderer& mockText() {
  static MockTextRenderer* renderer = nullptr;
  if (renderer == nullptr) {
    renderer = new MockTextRenderer();  // 故意不释放：见文件头注释
    FontCache::instance().setTextRenderer(renderer);
  }
  return *renderer;
}

// 统一 DPI：控件内部的 px() 在 96 DPI 下是恒等变换，期望值才好写。
// 焦点/悬停/布局测试都不该因为全局 DPI 残留而漂移，所以每个测试都调一次。
inline void ensureEnvironment() {
  mockText();
  setDesignDpi(96);
}

// ── 播放 ──

// 直接把控件摆到 size 并录一遍绘制命令。
// 不走 ViewRoot：命令测试要的是控件自己的意图，不希望混进容器的 translate。
inline std::vector<DrawCommand> recordDirect(View& view, Size size) {
  ensureEnvironment();
  RecordingPainter backend;
  // Painter 是面向控件的门面，RecordingPainter 只是它的后端。控件只认
  // Painter，所以这里必须包一层——直接传 backend 编不过，也不该编过。
  Painter painter(backend);
  view.layout(Rect::fromSize(size.width, size.height));
  view.draw(painter, Rect::fromSize(size.width, size.height));
  return backend.commands();
}

// 直接渲染到像素。Surface 初值全透明，所以「没画到」等价于 alpha=0，
// 断言「圆角外透明」「没有越界绘制」都靠这一点。
inline Surface renderDirect(View& view, Size size, bool antialias = true) {
  ensureEnvironment();
  Surface surface(size.width, size.height);
  SoftwareBackend::Options options;
  options.antialias = antialias;
  SoftwareBackend backend(surface, options);
  backend.setTextRenderer(&mockText());
  Painter painter(backend);
  view.layout(Rect::fromSize(size.width, size.height));
  view.draw(painter, Rect::fromSize(size.width, size.height));
  return surface;
}

// ── 像素查询 ──

// 非全透明像素的包围盒；全透明返回空矩形。
// 用它断言「画出来的东西正好占满控件」比逐像素比对更抗抗锯齿差异。
inline Rect alphaBounds(const Surface& surface) {
  int left = surface.width();
  int top = surface.height();
  int right = -1;
  int bottom = -1;
  for (int y = 0; y < surface.height(); ++y) {
    for (int x = 0; x < surface.width(); ++x) {
      if (colorAlpha(surface.at(x, y)) == 0) continue;
      if (x < left) left = x;
      if (y < top) top = y;
      if (x > right) right = x;
      if (y > bottom) bottom = y;
    }
  }
  if (right < 0) return Rect{};
  return Rect{left, top, right + 1, bottom + 1};
}

inline bool isTransparent(const Surface& surface, int x, int y) {
  return colorAlpha(surface.at(x, y)) == 0;
}

// 不比较 alpha 的颜色相等：抗锯齿边缘的 alpha 是中间值，颜色分量才是断言目标
inline bool sameRgb(Color a, Color b) {
  return colorRed(a) == colorRed(b) && colorGreen(a) == colorGreen(b) &&
         colorBlue(a) == colorBlue(b);
}

// ── 需要交互状态的场景 ──

// 把控件挂进一棵真实的树。悬停、按下、焦点都只能经 ViewRoot 设置，
// 直接改控件标志位在测试里做不到（它们是 private）。
class RootHarness {
 public:
  explicit RootHarness(Size size) : size_(size) {
    ensureEnvironment();
    auto frame = std::make_unique<FrameLayout>();
    host_ = frame.get();
    root_.setContentView(std::move(frame));
    root_.setSize(size);
    root_.setDpi(96);
    root_.performLayout();
  }

  // 新加的控件默认撑满宿主：单控件测试不需要再写布局参数
  template <typename T, typename... Args>
  T* add(Args&&... args) {
    T* view = host_->add<T>(std::forward<Args>(args)...);
    root_.performLayout();
    return view;
  }

  // 改过属性后重新布局：setter 只置「待重排」，不会立刻生效
  void relayout() { root_.performLayout(); }

  // 经真实树绘制：悬停/按下/焦点这些状态是 ViewRoot 维护的，
  // 只有走这条路径才能验证「状态变化确实改到了像素」。
  Surface render() {
    Surface surface(size_.width, size_.height);
    SoftwareBackend::Options options;
    options.antialias = true;
    SoftwareBackend backend(surface, options);
    backend.setTextRenderer(&mockText());
    Painter painter(backend);
    root_.drawAll(painter);
    return surface;
  }

  ViewRoot& root() { return root_; }
  FrameLayout* host() const { return host_; }
  Size size() const { return size_; }

 private:
  ViewRoot root_;
  FrameLayout* host_ = nullptr;
  Size size_{};
};

// ── 期望绘制命令的构造 ──
//
// 只填参与比较的字段。RecordingPainter 比较的是结构体全字段，
// 这里不填的字段保持 0，与录制端一致。
namespace cmd {

inline DrawCommand fillRect(Rect rc, Color color) {
  DrawCommand c;
  c.kind = DrawCommand::Kind::kFillRect;
  c.rect = rc;
  c.color = color;
  return c;
}

inline DrawCommand fillRound(Rect rc, Color color, int radius) {
  DrawCommand c;
  c.kind = DrawCommand::Kind::kFillRoundRect;
  c.rect = rc;
  c.color = color;
  c.radius = radius;
  return c;
}

inline DrawCommand strokeRound(Rect rc, Color color, int radius, int lineWidth = 1) {
  DrawCommand c;
  c.kind = DrawCommand::Kind::kStrokeRoundRect;
  c.rect = rc;
  c.color = color;
  c.radius = radius;
  c.lineWidth = lineWidth;
  return c;
}

inline DrawCommand gradientRound(Rect rc, Color top, Color bottom, int radius) {
  DrawCommand c;
  c.kind = DrawCommand::Kind::kGradientRoundRect;
  c.rect = rc;
  c.color = top;
  c.color2 = bottom;
  c.radius = radius;
  return c;
}

inline DrawCommand text(Rect rc, std::string content, Color color, int fontHeight,
                        TextAlign align = TextAlign::kLeft) {
  DrawCommand c;
  c.kind = DrawCommand::Kind::kText;
  c.rect = rc;
  c.color = color;
  c.text = std::move(content);
  c.fontHeight = fontHeight;
  c.intValue = static_cast<int>(align);
  return c;
}

inline DrawCommand textRun(Rect rc, std::string content, Color color, int fontHeight) {
  DrawCommand c;
  c.kind = DrawCommand::Kind::kTextRun;
  c.rect = rc;
  c.color = color;
  c.text = std::move(content);
  c.fontHeight = fontHeight;
  return c;
}

inline DrawCommand icon(IconId id, Rect rc, Color color) {
  DrawCommand c;
  c.kind = DrawCommand::Kind::kIcon;
  c.rect = rc;
  c.color = color;
  c.icon = id;
  return c;
}

inline DrawCommand save() {
  DrawCommand c;
  c.kind = DrawCommand::Kind::kSave;
  return c;
}

inline DrawCommand restore() {
  DrawCommand c;
  c.kind = DrawCommand::Kind::kRestore;
  return c;
}

inline DrawCommand clipRect(Rect rc) {
  DrawCommand c;
  c.kind = DrawCommand::Kind::kClipRect;
  c.rect = rc;
  return c;
}

}  // namespace cmd

// 命令序列比较：返回空串表示完全一致
inline std::string diffCommands(const std::vector<DrawCommand>& actual,
                                const std::vector<DrawCommand>& expected) {
  return RecordingPainter::diff(actual, expected);
}

// 在命令序列里找第一条指定类型；找不到返回 nullptr
inline const DrawCommand* firstOf(const std::vector<DrawCommand>& commands,
                                  DrawCommand::Kind kind) {
  for (const DrawCommand& command : commands) {
    if (command.kind == kind) return &command;
  }
  return nullptr;
}

inline int countOf(const std::vector<DrawCommand>& commands, DrawCommand::Kind kind) {
  int count = 0;
  for (const DrawCommand& command : commands) {
    if (command.kind == kind) ++count;
  }
  return count;
}

}  // namespace test
}  // namespace pyui

// 失败时把「第一条差异」直接打进断言信息里，省得再回头翻命令转储
#define CHECK_DRAW(actual, expected)                                          \
  do {                                                                        \
    const std::string d_ = ::pyui::test::diffCommands((actual), (expected));   \
    if (!d_.empty()) FAIL_AT(std::string("绘制命令不一致：") + d_);             \
  } while (false)

namespace pyui {
namespace test {

// ── 断言输出的可读化 ──

inline std::string show(const Rect& rc) {
  std::ostringstream out;
  out << '{' << rc.left << ',' << rc.top << ',' << rc.right << ',' << rc.bottom << '}';
  return out.str();
}

inline std::string show(const Size& size) {
  std::ostringstream out;
  out << size.width << 'x' << size.height;
  return out.str();
}

inline std::string show(Color color) {
  static const char* kDigits = "0123456789ABCDEF";
  std::string out = "#";
  for (int shift = 28; shift >= 0; shift -= 4) {
    out += kDigits[(color >> shift) & 0xFu];
  }
  return out;
}

}  // namespace test
}  // namespace pyui
