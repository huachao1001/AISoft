// StatusBar：规格胶囊优先于路径——窄窗口下不能让「1600×1000」先消失
//
// 这条守的是一个实际踩到的问题：分辨率、大小、序号从标题栏搬到状态栏后，
// 空间不足时最先被挤掉的若是胶囊，用户就再也看不到图片规格；而路径本来
// 就长得放不下，裁掉一截才合理。
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/font.h"
#include "AiUI/widgets/status_bar.h"

#include <string>
#include <vector>

namespace {

using namespace aiui_test;
using namespace AiUI;

struct BarEnv {
  ScopedFakePlatform fake;
  BarEnv() { fonts.create(96); }
  ~BarEnv() { fonts.destroy(); }
};

// onPaint 是 protected：用子类开一个只供测试的绘制入口
struct PaintableBar : StatusBar {
  void paintForTest(Painter& painter, const Rect& rect) { onPaint(painter, rect); }
};

std::wstring joinTexts(const std::vector<TextCall>& texts) {
  std::wstring all;
  for (const TextCall& call : texts) all += call.text;
  return all;
}

}  // namespace

// badges 全部画出来，且顺序跟塞进去的一致
AIUI_TEST(status_bar_draws_all_badges) {
  BarEnv env;
  PaintableBar bar;
  StatusFields fields;
  fields.tag = L"PNG";
  fields.badges = {L"1600×1000", L"8.4 KB", L"1/3"};
  bar.setFields(fields);

  RecordingBackend backend;
  Painter painter(backend);
  bar.paintForTest(painter, Rect{0, 0, 1200, 36});

  const std::wstring all = joinTexts(backend.texts());
  for (const wchar_t* text : {L"PNG", L"1600×1000", L"8.4 KB", L"1/3"}) {
    AIUI_CHECK(all.find(text) != std::wstring::npos);
  }
  // 顺序：tag 在 badges 之前
  AIUI_CHECK(all.find(L"PNG") < all.find(L"1600×1000"));
}

// 空串跳过，调用方按条件塞不会在画面上留空位
AIUI_TEST(status_bar_skips_empty_badges) {
  BarEnv env;
  PaintableBar bar;
  StatusFields fields;
  fields.badges = {L"", L"100×100", L""};
  bar.setFields(fields);

  RecordingBackend backend;
  Painter painter(backend);
  bar.paintForTest(painter, Rect{0, 0, 600, 36});

  // 只有一颗胶囊：圆角底一次 + 里面一段文字
  int pillCount = 0;
  for (const FillCall& fill : backend.fills()) {
    if (fill.radius > 0) ++pillCount;
  }
  AIUI_CHECK(pillCount == 1);
  AIUI_CHECK(joinTexts(backend.texts()).find(L"100×100") != std::wstring::npos);
}

// 路径再长也不能把胶囊挤掉：挤的时候裁的是路径
AIUI_TEST(status_bar_keeps_badges_when_path_is_long) {
  BarEnv env;
  PaintableBar bar;
  StatusFields fields;
  fields.left = L"C:\\Users\\someone\\Pictures\\2026\\very\\deep\\folder\\name\\photo.png";
  fields.badges = {L"1600×1000", L"8.4 KB", L"2/9"};
  fields.right = L"100%  ·  1234,567";
  bar.setFields(fields);

  RecordingBackend backend;
  Painter painter(backend);
  bar.paintForTest(painter, Rect{0, 0, 700, 36});

  const std::wstring all = joinTexts(backend.texts());
  for (const wchar_t* text : {L"1600×1000", L"8.4 KB", L"2/9"}) {
    AIUI_CHECK(all.find(text) != std::wstring::npos);
  }
  // 右侧统计也要保住
  AIUI_CHECK(all.find(L"1234,567") != std::wstring::npos);
  // 完整路径放不下：被裁过了
  AIUI_CHECK(all.find(fields.left) == std::wstring::npos);
}

// 极窄时宁可一条路径都不画，也不能用负宽度去画文字
AIUI_TEST(status_bar_drops_path_when_nothing_is_left) {
  BarEnv env;
  PaintableBar bar;
  StatusFields fields;
  fields.left = L"C:\\very\\long\\path\\photo.png";
  fields.badges = {L"1600×1000", L"8.4 KB"};
  bar.setFields(fields);

  RecordingBackend backend;
  Painter painter(backend);
  bar.paintForTest(painter, Rect{0, 0, 120, 36});

  for (const TextCall& call : backend.texts()) {
    AIUI_CHECK(call.rect.width() >= 0);
  }
}
