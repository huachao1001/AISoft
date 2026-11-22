// 绘制抽象
//
// 分两层：
//   PaintBackend —— 平台/光栅器只需实现这组最基础的图元
//   Painter      —— 面向控件的画笔：省略号、图标居中、棋盘格等组合算法
//                   写在这里，换平台时不必重写
//
// 控件绘制代码只依赖 Painter，不接触后端。
//
// 坐标系：Painter 的坐标是「当前视图局部坐标」。translate / save / restore
// 由后端维护变换栈，子视图绘制靠它把原点移到自己的左上角。
#pragma once

#include "pyui/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace pyui {

// 字体句柄：由文本后端创建，绘制时按值传递。不透明。
class Font {
 public:
  Font() = default;
  explicit Font(void* handle, int pixelHeight = 0) : handle_(handle), pixelHeight_(pixelHeight) {}
  void* handle() const { return handle_; }
  int pixelHeight() const { return pixelHeight_; }
  bool isValid() const { return handle_ != nullptr; }

 private:
  void* handle_ = nullptr;
  int pixelHeight_ = 0;
};

enum class TextAlign { kLeft, kCenter, kRight };

// 位图缩放时的采样方式。
// 放大到像素级时逐像素取整比插值更真实——用户要看清的是像素格，不是一团被
// 抹平的色块；其余场合用平滑采样，缩小时才不会出现锯齿摩尔纹。
enum class ImageSmoothing { kSmooth, kNearest };

// 位图在目标矩形里的放置方式。
//
// 与 ImageSmoothing 分开：smoothing 管「怎么采样」，这里管「画在多大」。
// 放在 painter.h 而不是 image_view.h，是因为它是「图像 → 矩形」的通用语义，
// 用 ImageView 之外的地方（如标题栏图标）也要能表达。
enum class ImageScale {
  kFit,      // 等比缩放，完整放进目标矩形（默认）
  kFill,     // 等比缩放填满目标矩形，超出部分裁掉
  kStretch,  // 拉伸到目标矩形，不保持比例
  kCenter,   // 原始大小居中
};

// 位图句柄：由图像后端产生，绘制时按值传递。不透明。
class Image {
 public:
  Image() = default;
  explicit Image(void* handle) : handle_(handle) {}
  void* handle() const { return handle_; }
  bool isValid() const { return handle_ != nullptr; }

 private:
  void* handle_ = nullptr;
};

// 矢量图标标识。控件只说画哪个图标，具体路径由绘制层提供。
enum class IconId {
  kNone,
  kCheck,
  kChevronDown,
  kChevronRight,
  kChevronUp,
  kMinimize,
  kMaximize,
  kRestore,
  kClose,
  kSearch,
  kFolder,
  kFile,
};

// ─────────────────────────────────────────────────────────────
// 平台/光栅器实现的最小图元集合
// ─────────────────────────────────────────────────────────────
class PaintBackend {
 public:
  virtual ~PaintBackend() = default;

  // ── 几何填充 ──
  virtual void fillRect(const Rect& rc, Color color) = 0;
  virtual void fillRoundRect(const Rect& rc, Color color, int radius) = 0;
  virtual void strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth) = 0;

  // 垂直渐变：top 对应 rc.top，bottom 对应 rc.bottom，含首末行。
  virtual void drawGradientRect(const Rect& rc, Color top, Color bottom) = 0;
  virtual void drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) = 0;

  // 棋盘格：透明区域的「没有像素」底纹。
  //
  // 图案相位锚在**坐标系原点**而不是 rc 的左上角：调用方就是要靠这个特性，
  // 让底纹在内容移动时保持不动（拖动图片时格子固定，才有放大镜的感觉）。
  // 若锚在 rc 上，每次 rc 一变图案就跟着平移，格子就「粘」在内容上了。
  //
  // 纯虚而不是给默认实现：默认实现只能退化成纯色填充，画出来的东西与
  // 约定的底纹不同，却不会有任何报错——这种「悄悄画错」正是铁律六要防的。
  virtual void fillCheckerboard(const Rect& rc, Color colorA, Color colorB, int cell) = 0;

  // ── 覆盖率蒙版 ──
  // 把一个 8 位覆盖率矩阵当作 alpha 用 color 混进当前表面。
  //
  // 这是**文字**的图元：字形栅格化出来就是「每个像素一个覆盖度」，没有
  // 别的形状能表达它。不给这个图元的话，文字后端只能逐像素调 fillRect，
  // 一句 20 字的中文就是几千次调用，每次都重做一遍裁剪求交。
  //
  // rc 与 coverage 的左上角对齐；stride 是每行字节数，允许大于 rc.width()
  // （GDI 的字形缓冲区按 DWORD 对齐，行尾有填充）。0..255 之外的语义由
  // 调用方保证，这里只负责相乘与混合。
  //
  // 默认实现逐像素退回 fillRect：正确但慢，只作为「别的后端还没优化」的
  // 保底，不是推荐路径。光栅器应当覆盖它。
  virtual void blendCoverageMask(const Rect& rc, const std::uint8_t* coverage, int stride,
                                 Color color);

  // ── 文本 ──
  // rc 为行框，单行垂直居中
  virtual void drawText(const Rect& rc, std::string_view text, Color color, const Font& font,
                        TextAlign align) = 0;
  // 按排版坐标绘制：原点即 rc.left，不加左右留白。
  // 光标/选区定位必须与它配合，否则会整体偏移半个留白。
  virtual void drawTextRun(const Rect& rc, std::string_view text, Color color, const Font& font) {
    drawText(rc, text, color, font, TextAlign::kLeft);
  }
  // 测宽：不依赖任何窗口，测量阶段直接可用。含安全余量，防末字被裁掉。
  virtual int measureText(std::string_view text, const Font& font) = 0;
  // 与绘制完全对齐的占位宽度，无安全余量。光标定位必须用它。
  virtual int advanceText(std::string_view text, const Font& font) { return measureText(text, font); }
  // 整串一次排版，返回每个字符的左右边界（长度 = size*2）。
  // 光标必须按整串定位：逐前缀调用 advanceText 会把每段各自的排版留白累加进来。
  virtual std::vector<int> glyphBounds(std::string_view text, const Font& font) {
    (void)text;
    (void)font;
    return {};
  }
  virtual int textHeight(const Font& font) = 0;
  // 行高（含行距），多行文本用
  virtual int lineHeight(const Font& font) { return textHeight(font); }

  // ── 图标与位图 ──
  // 图标字形：按墨迹框贴合 rc 居中
  virtual void drawIcon(IconId id, const Rect& rc, Color color) = 0;
  // 位图：等比缩放居中到 rc
  virtual void drawImage(const Image& image, const Rect& rc) = 0;
  // 只取 source 那一块（原图像素坐标）铺到 rc。放大查看时视口只覆盖图像的一小
  // 部分，整张铺过去会让后端白算看不见的区域。source 为空表示整张图。
  virtual void drawImage(const Image& image, const Rect& rc, const Rect& source) {
    (void)source;
    drawImage(image, rc);
  }
  // alpha 把整张图按比例淡下去（0 全透明、1 不透明）：缩放跨档位时新旧两种
  // 画法要交叉淡入淡出，就得能画半透明的图。
  virtual void drawImageScaled(const Image& image, const Rect& rc, const Rect& source,
                               ImageSmoothing smoothing, float alpha) {
    (void)smoothing;
    (void)alpha;
    drawImage(image, rc, source);
  }

  // ── 裁剪 ──
  virtual void setClip(const Rect& rc) = 0;
  virtual void resetClip() = 0;
  // 与当前裁剪区求交；配合 save/restore 逐层收缩
  virtual void clipRect(const Rect& rc) = 0;
  // 圆角裁剪：与当前裁剪区求交，但边界带圆角。
  // 用于「圆角容器里铺满整行的底色」——矩形裁剪会盖住容器的圆角。
  virtual void clipRoundRect(const Rect& rc, int radius) {
    (void)radius;
    clipRect(rc);
  }

  // ── 坐标变换与状态栈 ──
  virtual void translate(int dx, int dy) = 0;
  // 绕当前原点旋转（角度，顺时针为正）。图片按 90° 倍数旋转靠它，
  // 避免每次转角都重采样一遍像素。不支持时调用方自行降级。
  virtual void rotate(float degrees) { (void)degrees; }
  virtual void save() = 0;
  virtual void restore() = 0;

  // 当前变换后的裁剪区；绘制优化可以据此跳过不可见区域。
  // 返回空矩形表示「不知道」或「完全不可见」。默认返回不知道。
  virtual Rect currentClip() const { return Rect{}; }
};

// ─────────────────────────────────────────────────────────────
// 面向控件的画笔
// ─────────────────────────────────────────────────────────────
class Painter {
 public:
  explicit Painter(PaintBackend& backend) : backend_(backend) {}
  virtual ~Painter() = default;

  PaintBackend& backend() { return backend_; }

  // ── 几何 ──
  void fillRect(const Rect& rc, Color color) { backend_.fillRect(rc, color); }
  void fillRoundRect(const Rect& rc, Color color, int radius) {
    backend_.fillRoundRect(rc, color, radius);
  }
  void strokeRoundRect(const Rect& rc, Color color, int radius, int lineWidth = 1) {
    backend_.strokeRoundRect(rc, color, radius, lineWidth);
  }
  void drawGradientRect(const Rect& rc, Color top, Color bottom) {
    backend_.drawGradientRect(rc, top, bottom);
  }
  void drawGradientRoundRect(const Rect& rc, Color top, Color bottom, int radius) {
    backend_.drawGradientRoundRect(rc, top, bottom, radius);
  }
  void fillCheckerboard(const Rect& rc, Color colorA, Color colorB, int cell) {
    backend_.fillCheckerboard(rc, colorA, colorB, cell);
  }
  void drawHLine(int left, int right, int y, Color color) {
    backend_.fillRect(Rect{left, y, right, y + 1}, color);
  }
  void drawVLine(int top, int bottom, int x, Color color) {
    backend_.fillRect(Rect{x, top, x + 1, bottom}, color);
  }
  // 一像素边框：四条边分别填充，不依赖后端的描边实现
  void drawRectOutline(const Rect& rc, Color color, int width = 1);

  // ── 文本 ──
  void drawText(const Rect& rc, std::string_view text, Color color, const Font& font,
                TextAlign align = TextAlign::kLeft);
  // 尾部省略号：二分找最长可容纳前缀。
  //
  // align 必须跟着传进来：省略后的文本同样要按控件的对齐方式摆放。少了这个
  // 参数，居中/右对齐的标签一旦放不下就会突然跳到最左边，看起来像布局错乱。
  void drawTextEllipsis(const Rect& rc, std::string_view text, Color color, const Font& font,
                        TextAlign align = TextAlign::kLeft);
  // 中间省略号：保留首尾、省略中段。
  //
  // 文件名必须这样处理——尾部省略会把扩展名干掉，而「是不是 png」恰好是用户
  // 最先要认的信息；首部省略则认不出是哪份文件。两头各留一截才对。
  void drawTextMiddleEllipsis(const Rect& rc, std::string_view text, Color color,
                              const Font& font, TextAlign align = TextAlign::kLeft);
  int measureText(std::string_view text, const Font& font) {
    return backend_.measureText(text, font);
  }
  int advanceText(std::string_view text, const Font& font) {
    return backend_.advanceText(text, font);
  }
  int textHeight(const Font& font) { return backend_.textHeight(font); }
  void drawTextRun(const Rect& rc, std::string_view text, Color color, const Font& font);

  // ── 图标与位图 ──
  void drawIcon(IconId id, const Rect& rc, Color color) { backend_.drawIcon(id, rc, color); }
  void drawImage(const Image& image, const Rect& rc) { backend_.drawImage(image, rc); }
  void drawImage(const Image& image, const Rect& rc, const Rect& source) {
    backend_.drawImage(image, rc, source);
  }
  void drawImageScaled(const Image& image, const Rect& rc, const Rect& source,
                       ImageSmoothing smoothing, float alpha = 1.0f) {
    backend_.drawImageScaled(image, rc, source, smoothing, alpha);
  }

  // ── 裁剪与变换 ──
  void setClip(const Rect& rc) { backend_.setClip(rc); }
  void resetClip() { backend_.resetClip(); }
  void clipRect(const Rect& rc) { backend_.clipRect(rc); }
  void clipRoundRect(const Rect& rc, int radius) { backend_.clipRoundRect(rc, radius); }
  void translate(int dx, int dy) { backend_.translate(dx, dy); }
  void rotate(float degrees) { backend_.rotate(degrees); }
  void save() { backend_.save(); }
  void restore() { backend_.restore(); }
  Rect currentClip() const { return backend_.currentClip(); }

  // ── 常用组合：不再需要单独的后端图元 ──

  // 分隔线（水平）：1 像素，画在 y 上
  void drawSeparator(int left, int right, int y, Color color) {
    drawHLine(left, right, y, color);
  }

  // 圆角加描边：控件背景的典型组合。
  // 半径会自动夹到 min(w,h)/2（像素语义铁律 #4）。
  void drawRoundRectWithBorder(const Rect& rc, Color fill, Color border, int radius,
                               int borderWidth = 1);

 private:
  // 二分找最长可容纳前缀的字节长度（按 advanceText，不做字节切分）
  size_t fitPrefix(std::string_view text, int limit, const Font& font) const;
  size_t fitSuffix(std::string_view text, int limit, const Font& font) const;

  PaintBackend& backend_;
};

}  // namespace pyui

// UTF-8 工具已迁到 pyui/utf8.h：需要它的不只是绘制层，脚本层的
// len()/下标也要它，不该为此链入整个 painter。这里 include 一下，
// 让既有调用方（widgets / view / text_field）继续编译。
#include "pyui/utf8.h"
