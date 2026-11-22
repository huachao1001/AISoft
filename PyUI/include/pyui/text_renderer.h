// 文本渲染后端
//
// 这是三平台唯一「必须各写一份」的绘制部分。
//
// 为什么文字不能共享实现：字形栅格化、hinting、unicode shaping 依赖平台
// 字体服务（Windows 的 GDI/DirectWrite、macOS 的 CoreText、Linux 的
// FreeType+HarfBuzz）。自己实现这一层是多年工程，且永远跟不上系统。
//
// 但「度量与绘制必须同源」这条铁律跨平台一致：measureText / advanceText /
// drawText / drawTextRun 必须用同一套排版，否则输入框光标会与文字错开。
#pragma once

#include "pyui/painter.h"
#include "pyui/types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pyui {

// 字重与样式
struct FontSpec {
  std::string family;   // 空 = 用平台默认界面字体
  float pointSize = 9.0f;
  bool bold = false;
  bool italic = false;
  int dpi = 96;
};

// 一行文字排版后的度量。多行文本靠它算换行与总高。
struct TextMetrics {
  int width = 0;        // 绘制宽度（不含安全余量）
  int height = 0;       // 墨迹高度
  int lineHeight = 0;   // 行高（含行距），多行用
  int ascent = 0;       // 基线到行框顶
  int descent = 0;      // 基线到行框底
};

class TextRenderer {
 public:
  virtual ~TextRenderer() = default;

  // 创建/释放字体。返回的句柄交给 Font 包装。
  virtual Font createFont(const FontSpec& spec) = 0;
  virtual void destroyFont(const Font& font) = 0;

  // 度量。**必须与绘制同源**：measure 是所有排版判断的依据。
  virtual TextMetrics measure(std::string_view text, const Font& font) = 0;

  // 与绘制完全对齐的占位宽度，无安全余量。光标定位与点击命中必须用它。
  virtual int advance(std::string_view text, const Font& font) = 0;

  // 整串一次排版，返回每个字符的左右边界（长度 = 字符数*2）。
  // 光标必须按整串定位：逐前缀调用 advance 会把每段各自的排版留白累加进来，
  // 文本越长插入点离末字越远。返回空表示不支持，调用方自行退回逐前缀。
  virtual std::vector<int> glyphBounds(std::string_view text, const Font& font) {
    (void)text;
    (void)font;
    return {};
  }

  // 绘制到画布。rc 是行框，垂直居中。
  virtual void draw(PaintBackend& backend, const Rect& rc, std::string_view text, Color color,
                    const Font& font, TextAlign align) = 0;

  // 按排版坐标绘制：原点即 rc.left，不加左右留白。
  // 默认退回普通绘制；有编辑控件的平台应覆盖它。
  virtual void drawRun(PaintBackend& backend, const Rect& rc, std::string_view text, Color color,
                       const Font& font) {
    draw(backend, rc, text, color, font, TextAlign::kLeft);
  }

  // 系统默认界面字体族
  virtual const char* defaultFamily() const = 0;
};

// Mock 文本渲染器：固定步进 + 方块字形。
//
// 用途是**让像素级跨平台比对成为可能**：真实字形由平台字体决定，形状必然
// 不同；把文字形状这个变量固定住之后，剩下的几何/配色/对齐就可以逐像素比较。
// 只用于测试与离线预览，不用于正式界面。
class MockTextRenderer : public TextRenderer {
 public:
  explicit MockTextRenderer(float advanceRatio = 0.5f);

  Font createFont(const FontSpec& spec) override;
  void destroyFont(const Font& font) override;
  TextMetrics measure(std::string_view text, const Font& font) override;
  int advance(std::string_view text, const Font& font) override;
  std::vector<int> glyphBounds(std::string_view text, const Font& font) override;
  void draw(PaintBackend& backend, const Rect& rc, std::string_view text, Color color,
            const Font& font, TextAlign align) override;
  void drawRun(PaintBackend& backend, const Rect& rc, std::string_view text, Color color,
               const Font& font) override;
  const char* defaultFamily() const override { return "mock"; }

  // 每个字符的前进宽度系数（相对字体像素高度）
  void setAdvanceRatio(float ratio) { advanceRatio_ = ratio; }

  struct MockFont {
    int pixelHeight = 12;
    int lineHeight = 16;
  };

 private:
  int advanceOf(std::string_view text, const Font& font) const;

  float advanceRatio_ = 0.5f;
  // 字体句柄由这里自己分配并持有，进程内不需要释放（测试进程短命）。
  // 用 vector 而不是 map：字号种类只有个位数，顺序查找比哈希更快也更好调。
  mutable std::vector<MockFont> fonts_;
};

// 进程内字体缓存：FontSpec → Font。
//
// 字体创建要走平台字体服务（GDI 的 CreateFontIndirect、CoreText 的
// CTFontCreate…），每次绘制都新建会非常慢，而且句柄要在退出前统一释放。
// 控件只描述「9pt、粗体」，由这里换成句柄并缓存。
class FontCache {
 public:
  static FontCache& instance();

  // 绑定文本渲染器。平台初始化时调用一次；换渲染器会清空缓存。
  void setTextRenderer(TextRenderer* renderer);
  TextRenderer* textRenderer() const { return renderer_; }

  // 取字体。没有渲染器时返回无效 Font，调用方据此退回占位字形。
  Font get(const FontSpec& spec);
  void clear();

 private:
  FontCache() = default;
  ~FontCache();

  struct Entry {
    FontSpec spec;
    Font font;
  };
  std::vector<Entry> entries_;
  TextRenderer* renderer_ = nullptr;
};

// ── 测量入口 ──
//
// 控件在不持有 Painter 的时候（onMeasure）也要能测量，所以测量通过全局
// 字体缓存走同一个 TextRenderer。绘制时后端也委托同一个 renderer，
// 这就是「四处同源」的落点。
//
// 没有绑定 TextRenderer 时用一组确定性回退值（与 MockTextRenderer 一致），
// 这样无字体的布局测试仍然能得到稳定的尺寸，而不是 0 宽把控件挤没。
int measureTextWidth(std::string_view text, const Font& font);
int textAdvance(std::string_view text, const Font& font);
int textInkHeight(const Font& font);
int textLineHeight(const Font& font);

// 界面常用字号（96 DPI 设计值 → 由 spec.dpi 换算）
inline Font uiFont(float pointSize = 9.0f, bool bold = false, bool italic = false, int dpi = 96) {
  FontSpec spec;
  spec.pointSize = pointSize;
  spec.bold = bold;
  spec.italic = italic;
  spec.dpi = dpi;
  return FontCache::instance().get(spec);
}

}  // namespace pyui
