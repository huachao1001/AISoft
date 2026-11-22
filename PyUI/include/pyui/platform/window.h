// 原生窗口
//
// 整棵视图树共用一个窗口，所以这里只有一套回调、一个脏区、一个焦点。
// 子视图没有句柄，这正是「控件纯绘制」得以成立的前提（见 AGENTS.md 铁律二）。
//
// 两个刻意的分割：
//   1. WindowHost —— 平台把原生消息翻译成中性事件后回调宿主；
//   2. PlatformWindow —— 宿主反过来驱动窗口（改尺寸、失效、拖动）。
// 平台后端不得在翻译阶段做业务判断，判断都在 ViewRoot / 控件里。
#pragma once

#include "pyui/events.h"
#include "pyui/painter.h"
#include "pyui/types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pyui {

class Canvas;

// 非客户区命中结果：平台据此决定交给系统（拖动、缩放）还是客户区控件。
// 自绘标题栏靠它既保留系统拖动/贴边吸附，又能让按钮收到鼠标事件。
enum class HitTarget {
  kClient,
  kCaption,
  kLeft,
  kRight,
  kTop,
  kBottom,
  kTopLeft,
  kTopRight,
  kBottomLeft,
  kBottomRight,
  // 最大化按钮：交给系统后，悬停会弹出贴靠布局（Snap Layouts）
  kMaxButton,
};

// 窗口外观
struct WindowStyle {
  // 顶层窗口保留系统外框（投影、圆角、边缘缩放）；子窗口只铺满父客户区
  bool topLevel = false;
  bool resizable = true;
  bool minimizable = true;
  bool maximizable = true;
  // 自绘标题栏的视觉总高（96 DPI 设计值）。0 表示不划走任何非客户区。
  int captionHeight = 0;
  // 左/右/下三条边的缩放带宽度（96 DPI 设计值），贴外沿即触发
  int borderResizeWidth = 4;
  // 顶部缩放带高度（96 DPI 设计值）。顶部有标题栏，缩放带落在内侧，
  // 比其他三条边宽一点才好抓。
  int topResizeWidth = 8;
};

struct WindowParams {
  bool visible = true;
  bool tabStop = true;
  // 窗口类名，需在进程内唯一。空则后端自动生成。
  std::string className;
  std::string title;
  WindowStyle style;
  // 最小尺寸（96 DPI 设计值），0 表示不限制
  int minWidth = 0;
  int minHeight = 0;
  // 初始位置与尺寸（屏幕像素）；{0,0,0,0} 表示由系统决定。
  Rect bounds{};
  // 图标：本可执行文件里的图标资源 id。0 表示取 exe 主图标。
  int iconResourceId = 0;
};

// 窗口把原生事件翻译成中性事件后回调宿主。
class WindowHost {
 public:
  virtual ~WindowHost() = default;

  // 需要重绘；update 是本次要重画的窗口区域（客户区坐标）
  virtual void onExpose(const Rect& update) = 0;
  virtual void onResize(int width, int height) = 0;

  virtual void onMouseDown(const MouseEvent& e) = 0;
  virtual void onMouseUp(const MouseEvent& e) = 0;
  virtual void onMouseMove(const MouseEvent& e) = 0;
  virtual void onMouseLeave() = 0;
  virtual void onMouseWheel(const MouseEvent& e) = 0;
  virtual void onDoubleClick(const MouseEvent& e) = 0;
  virtual void onContextMenu(const MouseEvent& e) = 0;

  // 返回 true 表示已消费
  virtual bool onKeyDown(const Key& key, const Modifiers& mods) = 0;
  // 输入字符用 Unicode 码点，不用 wchar_t：
  // UTF-16 代理对在 BMP 之外需要两个 wchar_t，按码点传就不用管这层。
  virtual bool onChar(std::uint32_t codepoint) = 0;
  virtual void onFocusChanged(bool focused) = 0;
  virtual void onTimer() = 0;

  // 非客户区命中：窗口坐标。标题栏返回 kCaption、边框返回缩放方向，
  // 默认 kClient 表示全部按客户区处理。
  virtual HitTarget onHitTest(const Point& position) {
    (void)position;
    return HitTarget::kClient;
  }

  // 关闭请求（Alt+F4、系统菜单、平台侧关闭）；返回 true 表示已处理，
  // 平台不再销毁窗口。默认返回 false 走正常关闭。
  virtual bool onCloseRequest() { return false; }

  // 拖入文件（绝对路径，UTF-8）。空指针即关闭拖放。
  virtual void onFileDrop(const std::vector<std::string>& paths) { (void)paths; }
};

// 平台窗口：整棵视图树共用一个。
//
// createCanvas 与 invalidateRect 是脏区模型的两端：
// 宿主先 invalidateRect(局部更新)，平台攒批后回调 onExpose(合成区域)，
// 宿主再 createCanvas(该区域) 画进离屏位图，析构时自动呈现。
class PlatformWindow {
 public:
  virtual ~PlatformWindow() = default;

  virtual NativeHandle handle() const = 0;
  virtual Rect clientRect() const = 0;
  virtual int dpi() const = 0;

  // 离屏画布，只覆盖 area（窗口客户区坐标）；nullptr 表示当前无法绘制。
  // painter() 收的仍是客户区坐标：画布内部已把 area 原点对到离屏位图原点，
  // 调用方不必再减一次 area.left/top。
  virtual std::unique_ptr<Canvas> createCanvas(const Rect& area) = 0;

  virtual void setBounds(const Rect& bounds) = 0;
  // 含边框与标题栏的整窗矩形，居中与吸附用
  virtual Rect windowRect() const = 0;
  // 客户区左上的屏幕坐标；弹出菜单定位用
  virtual Point clientOrigin() const { return Point{windowRect().left, windowRect().top}; }
  virtual void show(bool visible) = 0;
  virtual void invalidate() = 0;
  // 只失效一块区域，用于悬停、选中这类局部更新
  virtual void invalidateRect(const Rect& rc) = 0;
  // 窗口背景色：窗口变大时系统先铺它，避免新增区域露出黑底
  virtual void setBackgroundColor(Color color) { (void)color; }
  virtual void setFocus() = 0;
  virtual void setCursorShape(CursorShape shape) = 0;
  virtual void captureMouse(bool capture) = 0;

  // 光标闪烁定时器：整棵树共用一个
  virtual void setBlinkTimer(bool on) = 0;
  // 一次性延时定时器：ms 毫秒后触发 onDelayTimer，再次调用会重新计时；
  // 0 表示取消。光标闪烁是全树共享的周期定时器，滚动条自动隐藏这类
  // 「稍后做一次」的需求需要独立的单次计时。
  virtual void setDelayTimer(int ms) { (void)ms; }
  // 让输入法候选框跟随光标（客户区坐标）
  virtual void setImeCaret(int x, int y) = 0;

  // ── 标题栏与窗口控制 ──
  virtual void setTitle(const std::string& title) = 0;
  virtual void minimize() = 0;
  // 最大化与还原之间切换
  virtual void toggleMaximize() = 0;
  virtual bool isMaximized() const = 0;
  // 全屏演示：去掉边框与标题栏，铺满当前显示器（含任务栏区域）。
  // 退出后回到进入前的样式与位置，最大化状态也一并保留。
  virtual void setFullscreen(bool fullscreen) { (void)fullscreen; }
  virtual bool isFullscreen() const { return false; }
  // 还原态下的外框矩形；最大化时也返回还原尺寸，用于持久化
  virtual Rect normalBounds() const { return windowRect(); }
  virtual void close() = 0;
  // 拖动移动与从边缘缩放，供自绘标题栏调用
  virtual void startDragMove() = 0;
  virtual void startDragResize(HitTarget edge) = 0;

  // ── 文件拖放 ──
  // 接受从资源管理器/访达拖入的文件。onDrop 收到绝对路径列表，
  // 在 UI 线程调用。传空函数即关闭拖放。
  virtual void setFileDropHandler(std::function<void(const std::vector<std::string>&)> fn) {
    (void)fn;
  }
};

// 创建窗口的工厂。各平台实现一份，放在 src/os/<platform>/。
PlatformWindow* createPlatformWindow(NativeHandle parent, const WindowParams& params,
                                     WindowHost& host);
void destroyPlatformWindow(PlatformWindow* window);

}  // namespace pyui
