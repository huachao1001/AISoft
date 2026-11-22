// Windows 文本引擎的契约测试（铁律七）。
//
// 这一份测的不是「字形好不好看」，而是三条**跨平台必须成立**的性质：
//
//   1. 四处同源：measure / advance / glyphBounds / drawRun 用同一个笔推进
//      循环，所以任何两处对同一串文字给出的宽度必须一致。
//   2. 起点即 rc.left：drawRun 不加左右留白。这是输入框光标与文字对齐的
//      前提，偏 1px 就会出现「光标离字半个格子」的经典 bug。
//   3. GRAY8 真的用上了：字缘必须有 0 与 255 之间的中间覆盖率。若退化成
//      1 位，9pt 中文会糊成锯齿，而这一条在别的平台上不会暴露。
//
// 只有 Windows 能跑：文件在非 Windows 构建里编成空翻译单元。
#include "test_framework.h"

#include "pyui/painter.h"
#include "pyui/software_backend.h"
#include "pyui/surface.h"
#include "pyui/testing/recording_painter.h"
#include "pyui/text_renderer.h"

#if defined(PYUI_OS_WINDOWS)

#include "gdi_text_renderer.h"

#include <string>
#include <vector>

namespace pyui {
namespace test {
namespace {

// 每个用例建一次渲染器，但字体通过 FontCache 共享；这里直接持有渲染器，
// 避免把 FontCache 的全局状态带进断言。
struct Fixture {
  GdiTextRenderer renderer;
  Font font;

  Fixture() {
    // 9pt @96dpi ≈ 12px，与 MockTextRenderer 的像素高度一致，
    // 这样「假字体通过了的布局」换成真字体也不会突然放不下。
    FontSpec spec;
    spec.pointSize = 9.0f;
    spec.dpi = 96;
    font = renderer.createFont(spec);
  }
  ~Fixture() { renderer.destroyFont(font); }
};

// 把绘制录成命令。用 RecordingPainter 是因为它不碰像素，
// 「位置对不对」在命令层就能判定，比数像素稳。
std::vector<DrawCommand> recordRun(GdiTextRenderer& renderer, const Rect& rc,
                                   std::string_view text, const Font& font) {
  RecordingPainter backend;
  renderer.drawRun(backend, rc, text, makeColor(0, 0, 0), font);
  return backend.commands();
}

}  // namespace

TEST(gdi_font_has_sane_metrics) {
  Fixture f;
  CHECK(f.font.isValid());
  if (!f.font.isValid()) return;

  const TextMetrics metrics = f.renderer.measure(std::string_view{}, f.font);
  CHECK(metrics.lineHeight > 0);
  CHECK(metrics.height > 0);
  CHECK(metrics.ascent > 0);
  // 行高不能小于墨迹高，否则两行会叠在一起（中文尤其明显）
  CHECK_MSG(metrics.lineHeight >= metrics.height, "行高必须不小于墨迹高");
  CHECK_EQ(f.font.pixelHeight(), 12);
}

TEST(gdi_measure_and_advance_agree) {
  Fixture f;
  if (!f.font.isValid()) return;

  const char* const samples[] = {"A", "Hello", "确定", "PyUI 布局样例", "a b c"};
  for (const char* sample : samples) {
    const int advance = f.renderer.advance(sample, f.font);
    const TextMetrics metrics = f.renderer.measure(sample, f.font);
    CHECK_MSG(advance > 0, std::string("advance 必须为正：") + sample);
    // measure 比 advance 多 1px 安全余量（末字墨迹可能探出单元格），
    // 除此之外两者必须完全相等——它们来自同一个笔推进循环。
    CHECK_MSG(metrics.width == advance + 1,
              std::string("measure 应等于 advance+1：") + sample + " measure=" +
                  std::to_string(metrics.width) + " advance=" + std::to_string(advance));
  }
}

TEST(gdi_empty_text_measures_zero) {
  Fixture f;
  if (!f.font.isValid()) return;

  CHECK_EQ(f.renderer.advance(std::string_view{}, f.font), 0);
  CHECK_EQ(f.renderer.measure(std::string_view{}, f.font).width, 0);
  // 空串仍要有行高：输入框没内容时高度不能塌
  CHECK(f.renderer.measure(std::string_view{}, f.font).lineHeight > 0);
}

TEST(gdi_glyph_bounds_match_advance) {
  Fixture f;
  if (!f.font.isValid()) return;

  const std::string text = "abc中文";
  const std::vector<int> bounds = f.renderer.glyphBounds(text, f.font);
  const size_t chars = utf8Length(text);
  CHECK_EQ(bounds.size(), chars * 2);
  if (bounds.size() != chars * 2) return;

  // 首字符左界是 0，末字符右界就是整串 advance —— 光标能落在两端
  CHECK_EQ(bounds.front(), 0);
  CHECK_EQ(bounds.back(), f.renderer.advance(text, f.font));

  // 边界必须单调不减，且左右界不交叉
  for (size_t i = 0; i + 1 < bounds.size(); i += 2) {
    CHECK_MSG(bounds[i] <= bounds[i + 1], "字符的右界不能小于左界");
    if (i + 2 < bounds.size()) {
      CHECK_MSG(bounds[i + 1] == bounds[i + 2], "相邻字符的边界必须首尾相接");
    }
  }
}

TEST(gdi_draw_run_origin_is_rc_left) {
  Fixture f;
  if (!f.font.isValid()) return;

  const std::string text = "Hello";
  const std::vector<DrawCommand> at0 = recordRun(f.renderer, Rect{0, 0, 200, 16}, text, f.font);
  const std::vector<DrawCommand> at50 = recordRun(f.renderer, Rect{50, 0, 250, 16}, text, f.font);

  CHECK(!at0.empty());
  CHECK_EQ(at0.size(), at50.size());
  if (at0.size() != at50.size()) return;

  // 把一个字形接一个字形画出来；起点平移 50 后每块蒙版必须**精确**平移 50。
  // 任何「额外留白」都会让差值不等于 50，这条断言就是用来抓那个的。
  for (size_t i = 0; i < at0.size(); ++i) {
    CHECK_EQ(static_cast<int>(at0[i].kind),
             static_cast<int>(DrawCommand::Kind::kCoverageMask));
    CHECK_EQ(at50[i].rect.left - at0[i].rect.left, 50);
    CHECK_EQ(at50[i].rect.right - at0[i].rect.right, 50);
    // 垂直方向不该受水平平移影响
    CHECK_EQ(at0[i].rect.top, at50[i].rect.top);
  }
}

TEST(gdi_draw_run_first_ink_starts_at_rc_left) {
  Fixture f;
  if (!f.font.isValid()) return;

  // 用一个左边距为 0 的字形（'H' 的左侧承载在 Segoe UI 下是 0 或 1），
  // 断言第一块墨迹不会跑到 rc.left 的左边超过 1px。
  const std::vector<DrawCommand> commands =
      recordRun(f.renderer, Rect{10, 0, 210, 16}, "H", f.font);
  CHECK(!commands.empty());
  if (commands.empty()) return;
  CHECK_MSG(commands.front().rect.left >= 10 - 1,
            "第一个字形不该跑到起点左侧（负左侧承载不应超过 1px）");
}

TEST(gdi_draw_run_stays_inside_line_box) {
  Fixture f;
  if (!f.font.isValid()) return;

  const Rect box{0, 0, 300, 16};
  const std::vector<DrawCommand> commands = recordRun(f.renderer, box, "Ag中文", f.font);
  CHECK(!commands.empty());
  for (const DrawCommand& command : commands) {
    // 行框 16 高、字 12px，墨迹必然落在框内。越界说明基线算错。
    CHECK_MSG(command.rect.top >= box.top - 1, "墨迹不能高于行框");
    CHECK_MSG(command.rect.bottom <= box.bottom + 1, "墨迹不能低于行框");
  }
}

TEST(gdi_align_matches_advance) {
  Fixture f;
  if (!f.font.isValid()) return;

  const std::string text = "Align";
  const int width = f.renderer.advance(text, f.font);
  const Rect box{0, 0, 200, 16};

  RecordingPainter left, center, right;
  f.renderer.draw(left, box, text, makeColor(0, 0, 0), f.font, TextAlign::kLeft);
  f.renderer.draw(center, box, text, makeColor(0, 0, 0), f.font, TextAlign::kCenter);
  f.renderer.draw(right, box, text, makeColor(0, 0, 0), f.font, TextAlign::kRight);

  CHECK_EQ(left.commands().size(), center.commands().size());
  if (left.commands().empty()) return;

  // 居中的起点比左对齐多 (200-width)/2，右对齐多 200-width
  const int dxCenter = center.commands().front().rect.left - left.commands().front().rect.left;
  const int dxRight = right.commands().front().rect.left - left.commands().front().rect.left;
  CHECK_EQ(dxCenter, (200 - width) / 2);
  CHECK_EQ(dxRight, 200 - width);
}

TEST(gdi_gray8_produces_antialiased_edges) {
  Fixture f;
  if (!f.font.isValid()) return;

  // 真的栅格化一遍，检查字缘存在中间覆盖率。
  // 若 GetGlyphOutline 退化成 1 位，像素 alpha 只会是 0 或 255。
  Surface surface(128, 24);
  SoftwareBackend backend(surface);
  f.renderer.draw(backend, Rect{0, 0, 128, 24}, "Ag", makeColor(0, 0, 0), f.font,
                  TextAlign::kLeft);

  int partial = 0;
  int fullyOpaque = 0;
  for (int y = 0; y < surface.height(); ++y) {
    for (int x = 0; x < surface.width(); ++x) {
      const int alpha = static_cast<int>(colorAlpha(surface.at(x, y)));
      if (alpha == 0) continue;
      if (alpha >= 255) {
        ++fullyOpaque;
      } else {
        ++partial;
      }
    }
  }
  CHECK_MSG(fullyOpaque > 0, "字形应该有实心笔画");
  CHECK_MSG(partial > 0, "字形必须有抗锯齿边缘（否则 GRAY8 没生效）");
}

TEST(gdi_coverage_mask_composites_with_background) {
  Fixture f;
  if (!f.font.isValid()) return;

  // 白底黑字：字缘像素应当是灰的，而不是非黑即白。
  Surface surface(64, 20);
  surface.clear(makeColor(255, 255, 255));
  SoftwareBackend backend(surface);
  f.renderer.draw(backend, Rect{0, 0, 64, 20}, "A", makeColor(0, 0, 0), f.font,
                  TextAlign::kLeft);

  bool sawGray = false;
  for (int y = 0; y < surface.height() && !sawGray; ++y) {
    for (int x = 0; x < surface.width(); ++x) {
      const Color c = surface.at(x, y);
      const int r = static_cast<int>(colorRed(c));
      if (r > 16 && r < 239) {
        sawGray = true;
        break;
      }
    }
  }
  CHECK_MSG(sawGray, "白底黑字的边缘必须出现中间灰，否则没有混合");
}

TEST(gdi_cjk_glyphs_are_wider_than_latin) {
  Fixture f;
  if (!f.font.isValid()) return;

  // 一个汉字在多数中文字体里是全角，明显宽于半个拉丁字母。
  // 这条断言能抓到「字体没选中、全部回退到同一个宽度」这类错。
  const int latin = f.renderer.advance("A", f.font);
  const int cjk = f.renderer.advance("确", f.font);
  CHECK_MSG(cjk > latin, "汉字的步进应当大于拉丁字母");
  CHECK_MSG(cjk >= f.font.pixelHeight() * 3 / 4, "汉字步进不应小于 3/4 字高");
}

TEST(gdi_glyph_cache_is_reused) {
  Fixture f;
  if (!f.font.isValid()) return;

  const size_t before = f.renderer.cachedGlyphBytes(f.font);
  f.renderer.advance("Hello", f.font);
  const size_t afterFirst = f.renderer.cachedGlyphBytes(f.font);
  f.renderer.advance("Hello", f.font);
  const size_t afterSecond = f.renderer.cachedGlyphBytes(f.font);

  CHECK_MSG(afterFirst > before, "首次排版应当缓存字形");
  // 第二次不应新增：字形按码点缓存，同串再来一次必须命中缓存
  CHECK_EQ(afterSecond, afterFirst);
}

TEST(gdi_invalid_font_degrades_without_crashing) {
  Fixture f;
  (void)f;
  const Font invalid;
  CHECK_EQ(f.renderer.advance("abc", invalid), 0);
  CHECK_EQ(f.renderer.measure("abc", invalid).width, 0);
  CHECK(f.renderer.glyphBounds("abc", invalid).empty());

  RecordingPainter backend;
  f.renderer.draw(backend, Rect{0, 0, 40, 16}, "abc", makeColor(0, 0, 0), invalid,
                  TextAlign::kLeft);
  CHECK(backend.commands().empty());
}

TEST(gdi_default_family_is_not_empty) {
  GdiTextRenderer renderer;
  const char* family = renderer.defaultFamily();
  CHECK(family != nullptr);
  if (family != nullptr) CHECK_MSG(family[0] != '\0', "默认字体名不能为空");
}

}  // namespace test
}  // namespace pyui

#else

// 非 Windows 构建：整个文件是空翻译单元。
// 故意留一个内部链接的符号，避免某些工具链对「空文件」报警告。
namespace pyui {
namespace test {
namespace {
const int kGdiTextRendererTestsSkipped = 1;
}
}  // namespace test
}  // namespace pyui

#endif
