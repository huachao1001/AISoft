// 平台后端接口
//
// 视图只通过这里与操作系统打交道。所有平台相关实现集中在 src/os/<platform>/，
// 换平台时新增一份实现即可，控件代码一行不用改。
//
// 关键约束：整棵视图树共用一个窗口。ViewRoot 是唯一持有 PlatformWindow 的
// 对象，子视图不创建任何原生句柄，因此这里不再提供“每个控件一个窗口”的入口。
#pragma once

#include "AiUI/events.h"
#include "AiUI/icon.h"
#include "AiUI/painter.h"
#include "AiUI/types.h"

#include <memory>
#include <string>
#include <vector>

namespace AiUI {

class Canvas;

// 非客户区命中结果：平台据此决定交给系统（拖动、缩放）还是客户区控件。
// 自绘标题栏靠它既能保留系统拖动/贴边吸附，又能让按钮收到鼠标事件。
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

// 窗口把原生事件翻译成中性事件后回调宿主。
// 整棵树共用一个窗口，所以这里只有一套回调。
class WindowHost {
 public:
  virtual ~WindowHost() = default;

  // 需要重绘；update 是本次要重画的窗口区域
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
  virtual bool onChar(wchar_t ch) = 0;
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
};

// 窗口外观属性
struct WindowStyle {
  // 顶层窗口保留系统外框（投影、圆角、边缘缩放）；子窗口只铺满父客户区
  bool topLevel = false;
  bool resizable = true;
  bool minimizable = true;
  bool maximizable = true;
  // 自绘标题栏的视觉总高（96 DPI 设计值）；0 表示等于逻辑高度。
  // 逻辑高度（按钮悬停底色与垂直居中的基准）由 TitleBar::setBandHeight 控制。
  int captionHeight = 0;
  // 左/右/下三条边的缩放带宽度（96 DPI 设计值），贴外沿即触发
  int borderResizeWidth = 4;
  // 顶部缩放带高度（96 DPI 设计值）。顶部有标题栏，缩放带落在内侧，
  // 比其他三条边宽一点才好抓。
  int topResizeWidth = 8;
};

// 窗口创建参数
struct WindowParams {
  bool visible = true;
  bool tabStop = true;
  std::wstring className;  // 窗口类名，需在进程内唯一
  std::wstring title;
  WindowStyle style;
  // 最小尺寸（96 DPI 设计值），0 表示不限制
  int minWidth = 0;
  int minHeight = 0;
};
// 平台窗口：整棵视图树共用一个
class PlatformWindow {
 public:
  virtual ~PlatformWindow() = default;

  virtual NativeHandle handle() const = 0;
  virtual Rect clientRect() const = 0;
  virtual int dpi() const = 0;

  // 离屏画布，只覆盖 area（窗口客户区坐标）；nullptr 表示当前无法绘制
  virtual std::unique_ptr<Canvas> createCanvas(const Rect& area) = 0;

  virtual void setBounds(const Rect& bounds) = 0;
  // 含边框与标题栏的整窗矩形，居中与吸附用
  virtual Rect windowRect() const = 0;
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
  // 让输入法候选框跟随光标（客户区坐标）
  virtual void setImeCaret(int x, int y) = 0;

  // ── 标题栏与窗口控制 ──
  virtual void setTitle(const std::wstring& title) = 0;
  virtual void minimize() = 0;
  // 最大化与还原之间切换
  virtual void toggleMaximize() = 0;
  virtual bool isMaximized() const = 0;
  virtual void close() = 0;
  // 拖动移动与从边缘缩放，供自绘标题栏调用
  virtual void startDragMove() = 0;
  virtual void startDragResize(HitTarget edge) = 0;
};

// 平台后端
class Platform {
 public:
  virtual ~Platform() = default;

  virtual PlatformWindow* createWindow(NativeHandle parent, const WindowParams& params,
                                       WindowHost& host) = 0;
  virtual void destroyWindow(PlatformWindow* window) = 0;

  virtual int screenDpi() = 0;

  // ── 文本与剪贴板 ──
  // 系统默认界面字体族；各平台自带的中文/西文界面字体不同
  virtual const wchar_t* defaultFontFamily() = 0;
  // 写入纯文本；成功返回 true
  virtual bool setClipboardText(const std::wstring& text) = 0;
  // 读纯文本；没有文本内容时返回空串
  virtual std::wstring clipboardText() = 0;
  // 是否同一条路径前缀（忽略大小写，按分隔符边界判断）
  virtual bool isPathPrefix(const std::wstring& prefix, const std::wstring& path) = 0;
  // 路径分隔符
  virtual wchar_t pathSeparator() = 0;

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
  // 本进程可执行文件的小图标；标题栏左侧显示用，与系统标题栏一致
  virtual Image appIcon() = 0;

  // ── 绘制子系统 ──
  virtual void startupDrawing() = 0;
  virtual void shutdownDrawing() = 0;
  virtual bool isDrawingReady() = 0;
};

// 当前平台后端；进程内唯一
Platform* platform();

// 测试用：替换当前平台后端，传 nullptr 恢复默认实现
void setPlatformForTesting(Platform* platform);

// 宿主启动/退出时调用
void startupPlatform();
void shutdownPlatform();
bool isPlatformReady();

// 模态循环：禁用 owner 并只在自己窗口存活期间跑消息循环。
// owner 为空或不属于本进程时按非模态处理，避免锁死外部界面。
void runModalLoop(NativeHandle modal, NativeHandle owner);

}  // namespace AiUI
