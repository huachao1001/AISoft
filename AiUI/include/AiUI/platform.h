// 平台后端接口
//
// 控件只通过这里与操作系统打交道。所有平台相关实现集中在 src/win32，
// 换平台时新增一份实现即可，控件代码一行不用改。
#pragma once

#include "AiUI/events.h"
#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/types.h"

#include <memory>
#include <string>

namespace AiUI {

class Canvas;

// 平台窗口把原生事件翻译成中性事件后回调宿主
class WindowHost {
 public:
  virtual ~WindowHost() = default;

  virtual void onPaint(Canvas& canvas) = 0;
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
  virtual bool onChar(wchar_t ch) = 0;
  virtual void onFocusChanged(bool focused) = 0;
  virtual void onTimer() = 0;
  virtual void onScroll(const ScrollEvent& e) = 0;
};

// 窗口创建参数
struct WindowParams {
  int id = 0;
  bool visible = true;
  bool tabStop = false;
  bool scrollable = false;
  std::wstring className;  // 窗口类名，需在进程内唯一
};

// 平台窗口：承载一个控件
class PlatformWindow {
 public:
  virtual ~PlatformWindow() = default;

  virtual NativeHandle handle() const = 0;
  virtual Rect clientRect() const = 0;
  virtual int dpi() const = 0;

  // 离屏画布；nullptr 表示当前无法绘制
  virtual std::unique_ptr<Canvas> createCanvas() = 0;
  // 直接往窗口画的画笔（用于测量等场景）
  virtual std::unique_ptr<Painter> createPainter() = 0;

  virtual void setBounds(const Rect& bounds) = 0;
  virtual void show(bool visible) = 0;
  virtual void invalidate() = 0;
  virtual void setFocus() = 0;
  virtual void setCursorShape(CursorShape shape) = 0;
  virtual void captureMouse(bool capture) = 0;

  // 滚动信息，单位物理像素
  virtual void setScrollInfo(int total, int page, int position) = 0;

  // 光标闪烁定时器
  virtual void setBlinkTimer(bool on) = 0;
  // 让输入法候选框跟随光标
  virtual void setImeCaret(int x, int y) = 0;
};

// 平台后端
class Platform {
 public:
  virtual ~Platform() = default;

  virtual PlatformWindow* createWindow(NativeHandle parent, const WindowParams& params,
                                       WindowHost& host) = 0;
  virtual void destroyWindow(PlatformWindow* window) = 0;

  virtual int screenDpi() = 0;

  // ── 字体 ──
  virtual Font createFont(const wchar_t* family, float pointSize, bool bold, int dpi) = 0;
  virtual void destroyFont(void* handle) = 0;
  // 文本像素宽度：不依赖任何窗口，测量阶段直接可用
  virtual int measureText(const std::wstring& text, const Font& font) = 0;

  // ── 图片 ──
  // 从文件解码位图；失败返回无效 Image
  virtual Image loadImage(const std::wstring& path) = 0;
  virtual Size imageSize(const Image& image) = 0;
  virtual void releaseImage(Image image) = 0;

  // ── 绘制子系统 ──
  virtual void startupDrawing() = 0;
  virtual void shutdownDrawing() = 0;
  virtual bool isDrawingReady() = 0;
};

// 当前平台后端；进程内唯一
Platform* platform();

// 宿主启动/退出时调用
void startupPlatform();
void shutdownPlatform();
bool isPlatformReady();

}  // namespace AiUI
