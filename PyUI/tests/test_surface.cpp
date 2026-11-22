// Surface 外部内存包装（窗口后端的核心机制）
//
// 为什么要单独测：这个方法让「屏幕绘制」与「离线渲染」共用同一个
// SoftwareBackend。它一旦算错 stride，屏幕上的内容会错位成斜条纹，
// 而离线渲染测试却全绿——因为离线那条路走的是自有缓冲。
#include "test_framework.h"

#include "pyui/painter.h"
#include "pyui/software_backend.h"
#include "pyui/surface.h"

#include <vector>

namespace pyui {
namespace test {
namespace {

// 只比 RGB：抗锯齿边缘的 alpha 是中间值，断言颜色时它会干扰判断。
bool sameRgb(Color a, Color b) {
  return colorRed(a) == colorRed(b) && colorGreen(a) == colorGreen(b) &&
         colorBlue(a) == colorBlue(b);
}

bool isTransparent(const Surface& surface, int x, int y) {
  return colorAlpha(surface.at(x, y)) == 0;
}

}  // namespace

TEST(surface_wrap_external_reads_writes_through) {
  std::vector<std::uint32_t> buffer(8 * 4, 0);

  Surface surface = Surface::wrapExternal(buffer.data(), 8, 4);
  CHECK(surface.valid());
  CHECK_EQ(surface.width(), 8);
  CHECK_EQ(surface.height(), 4);

  // 写入必须落到外部缓冲：这是「不拷贝」的定义
  surface.set(3, 2, makeColor(0x11, 0x22, 0x33));
  CHECK_EQ(buffer[2 * 8 + 3], makeColor(0x11, 0x22, 0x33));
  CHECK_EQ(surface.at(3, 2), makeColor(0x11, 0x22, 0x33));

  // 反过来：直接改外部缓冲，Surface 要立刻看到
  buffer[0] = makeColor(0xaa, 0xbb, 0xcc);
  CHECK_EQ(surface.at(0, 0), makeColor(0xaa, 0xbb, 0xcc));
}

TEST(surface_wrap_external_honors_stride) {
  // 4 像素宽、每行 8 像素（行尾有填充），共 3 行
  std::vector<std::uint32_t> buffer(8 * 3, 0xFFFFFFFFu);

  Surface surface = Surface::wrapExternal(buffer.data(), 4, 3, 8 * static_cast<int>(sizeof(std::uint32_t)));
  CHECK(surface.valid());

  // 第 1 行第 0 列应当落在 buffer[8]，不是 buffer[4]
  const Rect r = surface.row(1) - buffer.data() == 8 ? Rect{} : Rect{};
  (void)r;
  CHECK_EQ(static_cast<long long>(surface.row(1) - buffer.data()), 8LL);

  surface.set(0, 1, makeColor(1, 2, 3));
  CHECK_EQ(buffer[8], makeColor(1, 2, 3));
  // 行尾填充不能被写：第 1 行有效宽度是 4，索引 4..7 是填充
  CHECK_EQ(buffer[4], 0xFFFFFFFFu);
}

TEST(surface_wrap_external_rejects_degenerate_arguments) {
  std::vector<std::uint32_t> buffer(16, 0);
  CHECK(!Surface::wrapExternal(nullptr, 4, 4).valid());
  CHECK(!Surface::wrapExternal(buffer.data(), 0, 4).valid());
  CHECK(!Surface::wrapExternal(buffer.data(), 4, 0).valid());
  // 负 stride 与「不足一行」的 stride 都应退回紧密排列，而不是算错
  Surface tight = Surface::wrapExternal(buffer.data(), 4, 4, -1);
  CHECK(tight.valid());
  CHECK_EQ(static_cast<long long>(tight.row(1) - buffer.data()), 4LL);
}

TEST(surface_wrap_external_renders_like_owned_buffer) {
  // 同一棵内容分别画进「自有 Surface」与「外部包装」，像素必须逐位相同。
  // 这条断言就是「屏幕与内存同源」的可执行形式。
  const Rect area = Rect::fromSize(24, 16);
  const Color fill = makeColor(0x2f, 0x6f, 0xe8);

  Surface owned(area.width(), area.height());
  {
    SoftwareBackend backend(owned);
    Painter painter(backend);
    painter.fillRect(Rect{2, 2, 20, 12}, fill);
    painter.drawRectOutline(Rect{1, 1, 23, 15}, makeColor(0, 0, 0));
  }

  std::vector<std::uint32_t> buffer(static_cast<size_t>(area.width()) * area.height(), 0);
  Surface external = Surface::wrapExternal(buffer.data(), area.width(), area.height());
  {
    SoftwareBackend backend(external);
    Painter painter(backend);
    painter.fillRect(Rect{2, 2, 20, 12}, fill);
    painter.drawRectOutline(Rect{1, 1, 23, 15}, makeColor(0, 0, 0));
  }

  CHECK_EQ(external.countDifferences(owned), 0);
  CHECK_EQ(static_cast<long long>(external.contentHash()),
           static_cast<long long>(owned.contentHash()));
}

TEST(surface_wrap_external_hash_ignores_row_padding) {
  // 同一张图用紧密排列与带填充两种方式包装，内容哈希必须一致：
  // 否则窗口后端与离线渲染的基线永远对不上。
  const int width = 5;
  const int height = 3;
  std::vector<std::uint32_t> tight(static_cast<size_t>(width) * height);
  for (size_t i = 0; i < tight.size(); ++i) {
    tight[i] = static_cast<std::uint32_t>(0xFF000000u | i);
  }

  // 每行 8 元素：拷贝有效像素，填充区填一个「毒值」
  std::vector<std::uint32_t> padded(8 * height, 0xDEADBEEFu);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      padded[static_cast<size_t>(y) * 8 + x] = tight[static_cast<size_t>(y) * width + x];
    }
  }

  Surface a = Surface::wrapExternal(tight.data(), width, height);
  Surface b = Surface::wrapExternal(padded.data(), width, height, 8 * static_cast<int>(sizeof(std::uint32_t)));
  CHECK_EQ(static_cast<long long>(a.contentHash()), static_cast<long long>(b.contentHash()));
  CHECK_EQ(b.countDifferences(a), 0);
}

TEST(surface_clear_only_touches_visible_pixels) {
  // 清屏不能把行尾填充也刷掉：那块内存可能属于同一张 DIB 的下一行
  // （某些驱动会这样排布），刷掉它会抹掉别的区域。
  // 4 像素宽、每行 8 元素，所以第 1 行从 buffer[8] 开始。
  std::vector<std::uint32_t> buffer(8 * 2, 0xDEADBEEFu);
  Surface surface = Surface::wrapExternal(buffer.data(), 4, 2, 8 * static_cast<int>(sizeof(std::uint32_t)));

  surface.clear(makeColor(1, 1, 1));
  CHECK_EQ(buffer[0], makeColor(1, 1, 1));
  CHECK_EQ(buffer[3], makeColor(1, 1, 1));
  CHECK_EQ(buffer[4], 0xDEADBEEFu);  // 第 0 行的填充区保持原样
  CHECK_EQ(buffer[8], makeColor(1, 1, 1));  // 第 1 行第 0 列
  CHECK_EQ(buffer[11], makeColor(1, 1, 1));
  CHECK_EQ(buffer[12], 0xDEADBEEFu);  // 第 1 行的填充区
}

// ── 位图绘制 ──
//
// drawImage 曾经是空实现（只 ++unsupported_）。症状是「标题栏左上角的
// 应用图标永远不出现」：ImageView 拿到了 Image、也调了 drawImage，
// 但光栅器什么都没画，而且没有任何报错。位图路径必须真的落地。

TEST(surface_draw_image_blits_source_pixels) {
  Surface source(4, 4);
  source.clear(makeColor(0x20, 0x80, 0xE0));

  Surface output(8, 8);
  SoftwareBackend backend(output);
  Painter painter(backend);
  painter.drawImage(Image(&source), Rect{0, 0, 8, 8});

  // 缩放是最近邻，4x4 → 8x8 后整块都该是同一个颜色
  CHECK(sameRgb(output.at(0, 0), makeColor(0x20, 0x80, 0xE0)));
  CHECK(sameRgb(output.at(7, 7), makeColor(0x20, 0x80, 0xE0)));
  CHECK_EQ(backend.unsupportedPrimitives(), 0);
}

TEST(surface_draw_image_respects_source_alpha) {
  // 透明像素必须留空：图标靠 alpha 决定哪里是背景，全画出来会变成方块。
  Surface source(2, 1);
  source.set(0, 0, makeColor(0x00, 0xFF, 0x00, 0x00));  // 全透明绿
  source.set(1, 0, makeColor(0xFF, 0x00, 0x00, 0xFF));  // 不透明红

  Surface output(2, 1);
  SoftwareBackend backend(output);
  Painter painter(backend);
  painter.drawImage(Image(&source), Rect{0, 0, 2, 1});

  CHECK(isTransparent(output, 0, 0));
  CHECK(sameRgb(output.at(1, 0), makeColor(0xFF, 0x00, 0x00)));
}

TEST(surface_draw_image_clips_to_source_rect) {
  // source 为空表示整张；给出一块时只画那一块（放大查看某处时用）
  Surface source(4, 1);
  source.clear(makeColor(0x00, 0x00, 0x00));
  source.set(3, 0, makeColor(0xFF, 0xFF, 0xFF));

  Surface output(1, 1);
  SoftwareBackend backend(output);
  Painter painter(backend);
  // 只取最右一列
  painter.drawImage(Image(&source), Rect{0, 0, 1, 1}, Rect{3, 0, 4, 1});

  CHECK(sameRgb(output.at(0, 0), makeColor(0xFF, 0xFF, 0xFF)));
}

TEST(surface_draw_image_counts_invalid_handle_as_unsupported) {
  // 没有位图不是「静默跳过」：计数留着，便于排查「控件拿到了空 Image」。
  Surface output(4, 4);
  SoftwareBackend backend(output);
  Painter painter(backend);
  painter.drawImage(Image(), Rect{0, 0, 4, 4});
  CHECK_EQ(backend.unsupportedPrimitives(), 1);
}

}  // namespace test
}  // namespace pyui
