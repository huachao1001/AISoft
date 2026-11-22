#include "gdi_text_renderer.h"

#include "win_common.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace pyui {
namespace {

// GGO_GRAY8_BITMAP 返回的是 0..64 的 65 级灰度；我们统一扩到 0..255，
// 这样 blendCoverageMask 只有一种量纲。四舍五入而不是直接乘 4：
// 直接乘 4 会让 64 → 256 溢出到 0（恰好丢掉「完全不透明」那一级）。
std::uint8_t coverageFromGray(int value) {
  const int v = value < 0 ? 0 : (value > 64 ? 64 : value);
  return static_cast<std::uint8_t>((v * 255 + 32) / 64);
}

// 16.16 定点单位 1.0。GetGlyphOutline 要求显式传矩阵（不接受 nullptr），
// 传恒等矩阵即「不做剪切/旋转/缩放」。
const MAT2 kIdentityMat2 = {
    {0, 1},  // eM11 = 1.0
    {0, 0},  // eM12 = 0
    {0, 0},  // eM21 = 0
    {0, 1},  // eM22 = 1.0
};

// UTF-8 → 码点。返回消耗的字节数（>=1），非法序列返回 U+FFFD。
// 与核心层的 utf8* 辅助函数分开写：那些函数只切边界不解码，而这里需要
// 真正的码点去查字形。
size_t decodeUtf8(std::string_view text, size_t offset, std::uint32_t& codepoint) {
  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  if (lead < 0x80) {
    codepoint = lead;
    return 1;
  }
  auto cont = [&](size_t i) -> int {
    return i < text.size() ? (static_cast<unsigned char>(text[i]) & 0x3F) : -1;
  };

  if ((lead >> 5) == 0x6) {
    const int c1 = cont(offset + 1);
    if (c1 < 0) {
      codepoint = 0xFFFD;
      return 1;
    }
    codepoint = static_cast<std::uint32_t>(((lead & 0x1Fu) << 6) | c1);
    return 2;
  }
  if ((lead >> 4) == 0xE) {
    const int c1 = cont(offset + 1);
    const int c2 = cont(offset + 2);
    if (c1 < 0 || c2 < 0) {
      codepoint = 0xFFFD;
      return 1;
    }
    codepoint =
        static_cast<std::uint32_t>(((lead & 0x0Fu) << 12) | (c1 << 6) | c2);
    return 3;
  }
  if ((lead >> 3) == 0x1E) {
    const int c1 = cont(offset + 1);
    const int c2 = cont(offset + 2);
    const int c3 = cont(offset + 3);
    if (c1 < 0 || c2 < 0 || c3 < 0) {
      codepoint = 0xFFFD;
      return 1;
    }
    codepoint = static_cast<std::uint32_t>(((lead & 0x07u) << 18) | (c1 << 12) | (c2 << 6) | c3);
    return 4;
  }
  codepoint = 0xFFFD;
  return 1;
}

// 一个已缓存的字形。
//
// coverage 已压缩成「无行尾填充」的连续矩阵（stride == width），
// 这样内存占用最小，blendCoverageMask 直接 stride=width 即可。
struct Glyph {
  int advance = 0;     // 到下一个字符原点的距离（gmCellIncX）
  int left = 0;        // 位图左边相对笔位置
  int topFromBaseline = 0;  // 位图顶边相对基线的**向上**距离（gmptGlyphOrigin.y）
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> coverage;
};

struct GdiFont {
  HFONT hfont = nullptr;
  int pixelHeight = 0;
  int lineHeight = 0;
  int ascent = 0;
  int descent = 0;
  // 该字体的字形缓存。map 而不是 hash：字号种类少、码点插入频率高，
  // 有序 map 的迭代与诊断更直观，且这里的瓶颈在 GDI 调用而不是查找。
  std::map<std::uint32_t, Glyph> glyphs;
  size_t cachedBytes = 0;
};

GdiFont* fontOf(const Font& font) { return static_cast<GdiFont*>(font.handle()); }

// 共享的 GDI 内存 DC。
//
// GetGlyphOutline 需要一个「选了字体」的 DC，但不写任何像素。用一个全局
// 内存 DC 只做查询，比每字体建一个 DC 省掉一堆句柄；1×1 单色位图是
// 必需的——GDI 的某些旧实现会对没有位图的 DC 返回失败。
//
// 线程：与整个 UI 层一致，假定单线程调用。
struct GlyphContext {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HFONT selected = nullptr;

  GlyphContext() {
    dc = ::CreateCompatibleDC(nullptr);
    if (dc != nullptr) {
      bitmap = ::CreateBitmap(1, 1, 1, 1, nullptr);
      if (bitmap != nullptr) ::SelectObject(dc, bitmap);
    }
  }
  ~GlyphContext() {
    if (dc != nullptr) {
      if (selected != nullptr) ::SelectObject(dc, selected);
      if (bitmap != nullptr) ::DeleteObject(bitmap);
      ::DeleteDC(dc);
    }
  }

  bool valid() const { return dc != nullptr; }
};

GlyphContext& glyphContext() {
  static GlyphContext context;
  return context;
}

}  // namespace

GdiTextRenderer::GdiTextRenderer() {
  // 平台默认界面字体：从系统非客户区度量里取 lfMessageFont，这是资源管理器、
  // 设置页都在用的那一款（Win10/11 是 Segoe UI）。写死 "Microsoft YaHei"
  // 会在英文系统上看起来不搭，且中文缺字时不会自动回退。
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != 0) {
    defaultFamily_ = win32::toUtf8(metrics.lfMessageFont.lfFaceName);
  }
  if (defaultFamily_.empty()) defaultFamily_ = "Segoe UI";
}

GdiTextRenderer::~GdiTextRenderer() = default;

Font GdiTextRenderer::createFont(const FontSpec& spec) {
  GlyphContext& context = glyphContext();
  if (!context.valid()) return Font();

  // 点数 → 像素：1pt = 1/72 英寸，像素 = pt * dpi / 72，四舍五入。
  // 与 MockTextRenderer 用同一个公式，这样假字体与真字体的行框尺寸一致，
  // 布局测试的期望值不用为了换渲染器再算一遍。
  const int dpi = spec.dpi > 0 ? spec.dpi : 96;
  const int pixelHeight =
      static_cast<int>(spec.pointSize * static_cast<float>(dpi) / 72.0f + 0.5f);
  if (pixelHeight <= 0) return Font();

  LOGFONTW lf{};
  lf.lfHeight = -pixelHeight;  // 负值 = 字符高度，正值 = 单元格高度
  lf.lfWeight = spec.bold ? FW_BOLD : FW_NORMAL;
  lf.lfItalic = spec.italic ? TRUE : FALSE;
  lf.lfCharSet = DEFAULT_CHARSET;
  // 灰度抗锯齿：GetGlyphOutline 的 GRAY8 位图就是这个质量档位的产物。
  // 选 CLEARTYPE_QUALITY 也不会改变 GRAY8 的输出，但会让字形 hinting 走
  // 另一条路径，度量在极端字号下可能与实际显示差 1px。
  lf.lfQuality = ANTIALIASED_QUALITY;
  lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;

  const std::string family = spec.family.empty() ? defaultFamily_ : spec.family;
  const std::wstring wide = win32::toWide(family);
  // 字体名最长 LF_FACESIZE-1，超长会被 GDI 静默截断，这里先截，避免
  // 截断位置落在代理对中间（toWide 已经处理成合法 UTF-16）。
  const size_t copyCount = std::min(wide.size(), static_cast<size_t>(LF_FACESIZE - 1));
  std::copy_n(wide.c_str(), copyCount, lf.lfFaceName);
  lf.lfFaceName[copyCount] = L'\0';

  HFONT handle = ::CreateFontIndirectW(&lf);
  if (handle == nullptr) return Font();

  auto* font = new GdiFont();
  font->hfont = handle;
  font->pixelHeight = pixelHeight;

  // 从同一个 DC 取行高与基线。GetTextMetrics 按**当前选中字体**回答，
  // 所以必须真的 SelectObject 一次，不能只看 LOGFONT。
  HGDIOBJ previous = ::SelectObject(context.dc, handle);
  TEXTMETRICW tm{};
  const bool haveMetrics = ::GetTextMetricsW(context.dc, &tm) != 0;
  if (previous != nullptr && previous != HGDI_ERROR) ::SelectObject(context.dc, previous);

  if (haveMetrics) {
    font->ascent = static_cast<int>(tm.tmAscent);
    font->descent = static_cast<int>(tm.tmDescent);
    // 行高含外部行距：段间距要靠它撑开，不含的话两行中文会贴在一起。
    font->lineHeight = static_cast<int>(tm.tmHeight + tm.tmExternalLeading);
  } else {
    // 取不到度量时退化成「与 Mock 一致」的比例，布局至少是稳的。
    font->ascent = pixelHeight;
    font->descent = std::max(1, pixelHeight / 4);
    font->lineHeight = pixelHeight * 4 / 3;
  }
  if (font->lineHeight <= 0) font->lineHeight = pixelHeight;
  if (font->lineHeight < font->ascent + font->descent) {
    font->lineHeight = font->ascent + font->descent;
  }

  return Font(font, pixelHeight);
}

void GdiTextRenderer::destroyFont(const Font& font) {
  GdiFont* gdi = fontOf(font);
  if (gdi == nullptr) return;
  // 共享 DC 里可能还选着这个字体：先换掉再删，否则 DeleteObject 会失败
  // （被选中的 GDI 对象不能删除），句柄就泄漏了。
  GlyphContext& context = glyphContext();
  if (context.valid() && context.selected == gdi->hfont) {
    ::SelectObject(context.dc, context.bitmap);
    context.selected = nullptr;
  }
  if (gdi->hfont != nullptr) ::DeleteObject(gdi->hfont);
  delete gdi;
}

namespace {

// 取（并缓存）一个字形。任何失败都退化成「无墨迹、有步进」的占位，
// 保证笔推进不会停住——停住会让后续所有字符叠在一起。
const Glyph* glyphFor(GdiFont* font, std::uint32_t codepoint) {
  auto found = font->glyphs.find(codepoint);
  if (found != font->glyphs.end()) return &found->second;

  Glyph glyph;
  GlyphContext& context = glyphContext();
  if (context.valid()) {
    if (context.selected != font->hfont) {
      ::SelectObject(context.dc, font->hfont);
      context.selected = font->hfont;
    }

    GLYPHMETRICS metrics{};
    // 先取度量（不取位图）：空白字符没有位图，但仍有步进。
    const DWORD size = ::GetGlyphOutlineW(context.dc, codepoint, GGO_GRAY8_BITMAP, &metrics, 0,
                                          nullptr, &kIdentityMat2);
    if (size != GDI_ERROR) {
      glyph.advance = static_cast<int>(metrics.gmCellIncX);
      glyph.left = static_cast<int>(metrics.gmptGlyphOrigin.x);
      glyph.topFromBaseline = static_cast<int>(metrics.gmptGlyphOrigin.y);
      glyph.width = static_cast<int>(metrics.gmBlackBoxX);
      glyph.height = static_cast<int>(metrics.gmBlackBoxY);

      if (size > 0 && glyph.width > 0 && glyph.height > 0) {
        // GGO 的位图**按 DWORD 对齐**：行宽是 4 的倍数，行尾有填充。
        std::vector<std::uint8_t> raw(size);
        const DWORD got = ::GetGlyphOutlineW(context.dc, codepoint, GGO_GRAY8_BITMAP, &metrics,
                                             size, raw.data(), &kIdentityMat2);
        if (got != GDI_ERROR && got <= size) {
          const int srcStride = (glyph.width + 3) & ~3;
          glyph.coverage.resize(static_cast<size_t>(glyph.width) *
                                static_cast<size_t>(glyph.height));
          for (int row = 0; row < glyph.height; ++row) {
            const std::uint8_t* src = raw.data() + static_cast<size_t>(row) * srcStride;
            std::uint8_t* dst = glyph.coverage.data() + static_cast<size_t>(row) * glyph.width;
            for (int x = 0; x < glyph.width; ++x) dst[x] = coverageFromGray(src[x]);
          }
          font->cachedBytes += glyph.coverage.size();
        }
      }
    }
  }

  if (glyph.advance <= 0) {
    // 取不到字形（字体缺字、非 BMP 码点）：给一个半角步进，让文本仍然
    // 可读地排下去。故意不画豆腐块——GDI 的缺字行为本来就随字体而变，
    // 我们画一个自制方块反而会与系统其它地方不一致。
    glyph.advance = std::max(1, font->pixelHeight / 2);
    glyph.width = 0;
    glyph.height = 0;
    glyph.coverage.clear();
  }

  auto inserted = font->glyphs.emplace(codepoint, std::move(glyph));
  return &inserted.first->second;
}

}  // namespace

TextMetrics GdiTextRenderer::measure(std::string_view text, const Font& font) {
  TextMetrics metrics;
  GdiFont* gdi = fontOf(font);
  if (gdi == nullptr) return metrics;

  metrics.height = gdi->ascent + gdi->descent;
  metrics.lineHeight = gdi->lineHeight;
  metrics.ascent = gdi->ascent;
  metrics.descent = gdi->descent;

  // 宽度 = 笔推进总和，与 drawRun 用的是同一个循环。
  // 额外留 1px 安全余量：hinting 让某些字母的墨迹比 gmCellIncX 探出一点点，
  // 不留的话右对齐的标签末字会被裁掉半列。留白只加在 measure 上，
  // advance 保持「与绘制完全对齐」，光标才不会偏。
  int width = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t codepoint = 0;
    offset += decodeUtf8(text, offset, codepoint);
    width += glyphFor(gdi, codepoint)->advance;
  }
  metrics.width = text.empty() ? 0 : width + 1;
  return metrics;
}

int GdiTextRenderer::advance(std::string_view text, const Font& font) {
  GdiFont* gdi = fontOf(font);
  if (gdi == nullptr) return 0;

  int width = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t codepoint = 0;
    offset += decodeUtf8(text, offset, codepoint);
    width += glyphFor(gdi, codepoint)->advance;
  }
  return width;
}

std::vector<int> GdiTextRenderer::glyphBounds(std::string_view text, const Font& font) {
  std::vector<int> bounds;
  GdiFont* gdi = fontOf(font);
  if (gdi == nullptr) return bounds;

  // 整串一次推进，返回每字符的左右边界。输入框的光标必须按这张表定位：
  // 逐前缀调 advance 会把每段的 hinting 舍入各自累加，文本越长偏得越多。
  const size_t count = utf8Length(text);
  bounds.reserve(count * 2);

  int pen = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t codepoint = 0;
    offset += decodeUtf8(text, offset, codepoint);
    const Glyph* glyph = glyphFor(gdi, codepoint);
    bounds.push_back(pen);
    pen += glyph->advance;
    bounds.push_back(pen);
  }
  return bounds;
}

void GdiTextRenderer::drawRun(PaintBackend& backend, const Rect& rc, std::string_view text,
                              Color color, const Font& font) {
  GdiFont* gdi = fontOf(font);
  if (gdi == nullptr || text.empty() || rc.isEmpty()) return;

  // 行框内垂直居中：把「上缘 + 行高」整体居中，基线在行高内偏上 ascent 处。
  // 用 lineHeight 而不是 ascent+descent：多出来的外部行距要均分到上下，
  // 否则单行文字在按钮里会看起来偏上。
  const int lineTop = rc.top + (rc.height() - gdi->lineHeight) / 2;
  const int baseline = lineTop + gdi->ascent;

  int pen = rc.left;
  size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t codepoint = 0;
    offset += decodeUtf8(text, offset, codepoint);
    const Glyph* glyph = glyphFor(gdi, codepoint);

    if (glyph->width > 0 && glyph->height > 0 && !glyph->coverage.empty()) {
      const Rect dst{pen + glyph->left, baseline - glyph->topFromBaseline,
                     pen + glyph->left + glyph->width,
                     baseline - glyph->topFromBaseline + glyph->height};
      backend.blendCoverageMask(dst, glyph->coverage.data(), glyph->width, color);
    }
    pen += glyph->advance;
  }
}

void GdiTextRenderer::draw(PaintBackend& backend, const Rect& rc, std::string_view text,
                           Color color, const Font& font, TextAlign align) {
  if (text.empty() || rc.isEmpty()) return;

  // 对齐只改起点，实际绘制仍然走 drawRun —— 排序与基线只有一处实现，
  // 这就是「四处同源」在绘制侧的落点。
  const int width = advance(text, font);
  int left = rc.left;
  if (align == TextAlign::kCenter) {
    left = rc.left + (rc.width() - width) / 2;
  } else if (align == TextAlign::kRight) {
    left = rc.right - width;
  }
  drawRun(backend, Rect{left, rc.top, left + width, rc.bottom}, text, color, font);
}

const char* GdiTextRenderer::defaultFamily() const { return defaultFamily_.c_str(); }

size_t GdiTextRenderer::cachedGlyphBytes(const Font& font) const {
  const GdiFont* gdi = fontOf(font);
  return gdi != nullptr ? gdi->cachedBytes : 0;
}

}  // namespace pyui
