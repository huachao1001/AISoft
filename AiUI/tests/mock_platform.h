// 假平台后端：不建真窗口、不碰 GDI+，只记录绘制调用与窗口状态
//
// 用途是让 View / ViewGroup / ViewRoot / ScrollView 的逻辑能在无桌面环境下
// 验证：几何、命中、焦点、脏区、滚动条位置、绘制平移与裁剪。
// 记录坐标一律是"窗口坐标"（即 Painter 当前坐标系），与实际后端一致。
#pragma once

#include "AiUI/platform.h"

#include <memory>
#include <vector>

namespace aiui_test {

using AiUI::Color;
using AiUI::Rect;
using AiUI::Size;

// 一次填充记录，rect 已叠加过当时的平移量
struct FillCall {
  Rect rect;
  Color color;
  // 圆角填充带上半径，用来区分「铺满矩形」与「带圆角的底」
  int radius = 0;
};

// 最小绘制后端：只记录填充与裁剪，其余图元忽略
class RecordingBackend : public AiUI::PaintBackend {
 public:
  void fillRect(const Rect& rc, Color color) override;
  void fillRoundRect(const Rect& rc, Color color, int radius) override;
  void strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth) override;
  void drawGradientRect(const Rect& rc, Color top, Color bottom) override;
  void drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) override;
  void drawText(const Rect& rc, const std::wstring& text, Color color, const AiUI::Font& font,
                AiUI::TextAlign align) override;
  int measureText(const std::wstring& text, int from, int length, const AiUI::Font& font) override;
  int textHeight(const AiUI::Font& font) override;
  void drawIcon(AiUI::IconId id, const Rect& rc, Color color) override;
  void drawImage(const AiUI::Image& image, const Rect& rc) override;
  void setClip(const Rect& rc) override;
  void resetClip() override;
  void clipRect(const Rect& rc) override;
  void clipRoundRect(const Rect& rc, int radius) override;
  void translate(int dx, int dy) override;
  void save() override;
  void restore() override;

  const std::vector<FillCall>& fills() const { return fills_; }
  void clear() { fills_.clear(); }
  int translateX() const { return tx_; }
  int translateY() const { return ty_; }
  // 当前裁剪区，空表示不限
  const Rect& clip() const { return clip_; }

 private:
  // 把局部坐标映射到当前平移后的坐标
  Rect map(const Rect& rc) const { return rc.moved(tx_, ty_); }

  struct State {
    int tx;
    int ty;
    Rect clip;
  };

  std::vector<FillCall> fills_;
  std::vector<State> stack_;
  int tx_ = 0;
  int ty_ = 0;
  Rect clip_{};
};

// 画布：记录被要求绘制的区域与提交次数
// 画布本身由 ViewRoot 持有并很快销毁，所以提交时把结果快照给窗口对象，
// 测试才能在 onExpose 返回后继续观察。
class FakeCanvas : public AiUI::Canvas {
 public:
  FakeCanvas(RecordingBackend& backend, const Rect& area) : backend_(backend), area_(area) {
    painter_ = std::make_unique<AiUI::Painter>(backend_);
  }

  AiUI::Painter& painter() override { return *painter_; }
  void commit() override;

  RecordingBackend& backend() { return backend_; }
  const Rect& area() const { return area_; }
  int commits() const { return commits_; }
  void setOwner(class FakeWindow* owner) { owner_ = owner; }

 private:
  RecordingBackend& backend_;
  Rect area_;
  std::unique_ptr<AiUI::Painter> painter_;
  class FakeWindow* owner_ = nullptr;
  int commits_ = 0;
};

class FakeWindow : public AiUI::PlatformWindow {
 public:
  FakeWindow(AiUI::WindowHost& host, int dpi) : host_(host), dpi_(dpi) {}

  AiUI::NativeHandle handle() const override { return const_cast<FakeWindow*>(this); }
  Rect clientRect() const override { return rect_; }
  int dpi() const override { return dpi_; }

  std::unique_ptr<AiUI::Canvas> createCanvas(const Rect& area) override;
  void setBounds(const Rect& bounds) override;
  Rect windowRect() const override { return rect_; }
  void show(bool visible) override { visible_ = visible; }
  void invalidate() override { ++fullInvalidations_; }
  void invalidateRect(const Rect& rc) override { invalidRects_.push_back(rc); }
  void setFocus() override { ++focusCalls_; }
  void setCursorShape(AiUI::CursorShape shape) override { cursor_ = shape; }
  void captureMouse(bool capture) override { captured_ = capture; }
  void setBlinkTimer(bool on) override { blink_ = on; }
  void setDelayTimer(int ms) override { delayMs_ = ms; }
  void setImeCaret(int x, int y) override {
    imeX_ = x;
    imeY_ = y;
  }

  // ── 标题栏控制：记录调用，测试据此断言 ──
  void setTitle(const std::wstring& title) override { title_ = title; }
  void minimize() override { ++minimizeCalls_; }
  void toggleMaximize() override {
    maximized_ = !maximized_;
    ++toggleCalls_;
  }
  bool isMaximized() const override { return maximized_; }
  void close() override { ++closeCalls_; }
  void startDragMove() override { ++dragMoveCalls_; }
  void startDragResize(AiUI::HitTarget edge) override { resizeEdge_ = edge; }

  // ── 供测试观察的状态 ──
  const std::vector<Rect>& invalidRects() const { return invalidRects_; }
  void clearInvalidRects() { invalidRects_.clear(); }
  int fullInvalidations() const { return fullInvalidations_; }
  bool captured() const { return captured_; }
  bool blink() const { return blink_; }
  AiUI::CursorShape cursor() const { return cursor_; }
  int focusCalls() const { return focusCalls_; }
  int imeX() const { return imeX_; }
  int imeY() const { return imeY_; }
  const std::wstring& title() const { return title_; }
  int minimizeCalls() const { return minimizeCalls_; }
  int toggleMaximizeCalls() const { return toggleCalls_; }
  int closeCalls() const { return closeCalls_; }
  int dragMoveCalls() const { return dragMoveCalls_; }
  AiUI::HitTarget resizeEdge() const { return resizeEdge_; }

  // 最近一次提交的绘制结果（画布销毁后仍可查）
  const std::vector<FillCall>& lastFills() const { return lastFills_; }
  const Rect& lastArea() const { return lastArea_; }
  int lastCommits() const { return lastCommits_; }
  int translateXAfterDraw() const { return lastTranslateX_; }
  int translateYAfterDraw() const { return lastTranslateY_; }
  void recordCommit(const RecordingBackend& backend, const Rect& area, int commits);

 private:
  AiUI::WindowHost& host_;
  Rect rect_{};
  int dpi_ = 96;
  bool visible_ = false;
  bool captured_ = false;
  bool blink_ = false;
  int delayMs_ = -1;
  int focusCalls_ = 0;
  int fullInvalidations_ = 0;
  int imeX_ = 0;
  int imeY_ = 0;
  AiUI::CursorShape cursor_ = AiUI::CursorShape::kArrow;
  std::vector<Rect> invalidRects_;
  std::wstring title_;
  bool maximized_ = false;
  int minimizeCalls_ = 0;
  int toggleCalls_ = 0;
  int closeCalls_ = 0;
  int dragMoveCalls_ = 0;
  AiUI::HitTarget resizeEdge_ = AiUI::HitTarget::kClient;
  // 最近一次提交的快照，避免画布销毁后指针悬空
  RecordingBackend backend_;
  std::vector<FillCall> lastFills_;
  Rect lastArea_{};
  int lastCommits_ = 0;
  int lastTranslateX_ = 0;
  int lastTranslateY_ = 0;
};

class FakePlatform : public AiUI::Platform {
 public:
  AiUI::PlatformWindow* createWindow(AiUI::NativeHandle parent, const AiUI::WindowParams& params,
                                     AiUI::WindowHost& host) override;
  void destroyWindow(AiUI::PlatformWindow* window) override;

  int screenDpi() override { return dpi_; }
  Rect workArea() override { return Rect{0, 0, 1920, 1080}; }
  int runEventLoop() override { return 0; }
  void quitEventLoop(int exitCode) override { (void)exitCode; }

  const wchar_t* defaultFontFamily() override { return L"Test Sans"; }
  bool setClipboardText(const std::wstring& text) override {
    clipboard_ = text;
    return true;
  }
  std::wstring clipboardText() override { return clipboard_; }
  bool isPathPrefix(const std::wstring& prefix, const std::wstring& path) override {
    // 与 Windows 语义一致：不区分大小写、按分隔符边界
    if (prefix.empty()) return true;
    if (path.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
      if (towlower(path[i]) != towlower(prefix[i])) return false;
    }
    if (path.size() == prefix.size()) return true;
    if (prefix.back() == L'\\' || prefix.back() == L'/') return true;
    return path[prefix.size()] == L'\\' || path[prefix.size()] == L'/';
  }
  wchar_t pathSeparator() override { return L'\\'; }

  AiUI::Font createFont(const wchar_t* family, float pointSize, bool bold, int dpi) override;
  void destroyFont(void* handle) override { (void)handle; }
  int measureText(const std::wstring& text, const AiUI::Font& font) override;
  // 模拟真实后端：字形边界是精确的等宽推进
  std::vector<int> glyphBounds(const std::wstring& text, const AiUI::Font& font) override;
  // 模拟真实后端的排版留白：多算的量随文本变长累积，
  // 谁把它用在光标定位上，长文本的插入点就会被推远
  int advanceText(const std::wstring& text, const AiUI::Font& font) override;
  AiUI::Image loadImage(const std::wstring& path) override {
    (void)path;
    return AiUI::Image();
  }
  AiUI::Size imageSize(const AiUI::Image& image) override {
    (void)image;
    return AiUI::Size{};
  }
  void releaseImage(AiUI::Image image) override { (void)image; }
  AiUI::Image appIcon() override { return AiUI::Image(); }
  void startupDrawing() override { drawingReady_ = true; }
  void shutdownDrawing() override { drawingReady_ = false; }
  bool isDrawingReady() override { return drawingReady_; }

  int64_t monotonicMillis() override { return monotonicMs_; }
  // 测试直接推进时间，不依赖真实时钟
  void advanceMillis(int64_t ms) { monotonicMs_ += ms; }

  void setDpi(int dpi) { dpi_ = dpi; }
  // 每个字符的宽度，默认 7，避免测试依赖真实字体
  void setCharWidth(int width) { charWidth_ = width; }
  // 模拟后端给出离谱的字符边界（GDI+ 超过 32 个范围时会整串退化）
  void setBrokenGlyphBounds(bool broken) { brokenGlyphBounds_ = broken; }
  int charWidth() const { return charWidth_; }
  FakeWindow* lastWindow() const { return lastWindow_; }
  int destroyedWindows() const { return destroyed_; }
  // 上次 createWindow 收到的类名
  const std::wstring& lastName() const { return lastName_; }

 private:
  int dpi_ = 96;
  int charWidth_ = 7;
  bool drawingReady_ = false;
  bool brokenGlyphBounds_ = false;
  int destroyed_ = 0;
  int64_t monotonicMs_ = 0;
  FakeWindow* lastWindow_ = nullptr;
  std::wstring lastName_;
  std::wstring clipboard_;
};

// 用法：作用域内把 platform() 换成 fake，析构时恢复
class ScopedFakePlatform {
 public:
  ScopedFakePlatform();
  ~ScopedFakePlatform();

  FakePlatform& platform() { return fake_; }

 private:
  FakePlatform fake_;
};

}  // namespace aiui_test
