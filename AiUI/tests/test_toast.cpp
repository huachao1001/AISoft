// Toast 的行为：淡入淡出时序、不吃鼠标、只显示一条
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/font.h"
#include "AiUI/toast.h"
#include "AiUI/view_root.h"
#include "AiUI/layouts/frame_layout.h"
#include <string>

namespace {

using namespace aiui_test;
using namespace AiUI;

struct ToastEnv {
  ScopedFakePlatform fake;
  ToastEnv() { fonts.create(96); }
  ~ToastEnv() { fonts.destroy(); }
};

const Rect kArea{0, 0, 800, 600};

// 画一次并返回记录的文本
std::wstring paintText(const Toast& toast) {
  RecordingBackend backend;
  Painter painter(backend);
  toast.paint(painter, kArea, 96);
  if (backend.texts().empty()) return std::wstring();
  return backend.texts().back().text;
}

}  // namespace

// 刚弹出时是全透明到不透明的起点，过完 fadeMs 才完全不透明
AIUI_TEST(toast_fades_in_then_holds) {
  ToastEnv env;
  Toast toast;
  toast.setTiming(Toast::Timing{1000, 100});

  AIUI_CHECK(!toast.isVisible());
  toast.show(L"已复制");
  AIUI_CHECK(toast.isVisible());
  AIUI_CHECK(toast.bounds(kArea, 96).isEmpty() == false);

  // 起点：alpha 为 0，画不出字
  AIUI_CHECK(paintText(toast).empty());

  // 淡入过半：已经有字了
  env.fake.platform().advanceMillis(50);
  AIUI_CHECK(paintText(toast) == L"已复制");

  // 淡入结束进入 hold：仍然可见
  env.fake.platform().advanceMillis(60);
  toast.tick(env.fake.platform().monotonicMillis());
  AIUI_CHECK(toast.isVisible());
  AIUI_CHECK(paintText(toast) == L"已复制");
}

// hold 结束后进入淡出，走完 fadeMs 就彻底收起
AIUI_TEST(toast_fades_out_and_hides) {
  ToastEnv env;
  Toast toast;
  toast.setTiming(Toast::Timing{200, 100, 100});
  toast.show(L"已复制");
  env.fake.platform().advanceMillis(100);  // 淡入 + 上浮结束
  toast.tick(env.fake.platform().monotonicMillis());
  env.fake.platform().advanceMillis(200);  // hold 结束
  toast.tick(env.fake.platform().monotonicMillis());

  // 淡出中途：还在，但已经不是满不透明
  env.fake.platform().advanceMillis(50);
  AIUI_CHECK(toast.isVisible());
  AIUI_CHECK(paintText(toast) == L"已复制");

  // 淡出走完：收起，且不再需要唤醒
  env.fake.platform().advanceMillis(60);
  const int64_t wake = toast.tick(env.fake.platform().monotonicMillis());
  AIUI_CHECK(!toast.isVisible());
  AIUI_CHECK_EQ(wake, static_cast<int64_t>(-1));
  AIUI_CHECK(toast.bounds(kArea, 96).isEmpty());
}

// 新消息顶掉旧消息并重新淡入：同一时刻只显示一条
AIUI_TEST(toast_new_message_replaces_old) {
  ToastEnv env;
  Toast toast;
  toast.setTiming(Toast::Timing{1000, 100});

  toast.show(L"第一条");
  env.fake.platform().advanceMillis(60);
  toast.show(L"第二条");

  // 重新从透明开始：此刻还没到可见
  AIUI_CHECK(paintText(toast).empty());
  env.fake.platform().advanceMillis(100);
  AIUI_CHECK(paintText(toast) == L"第二条");
}

// 提示条贴在底部居中，且不超出客户区
AIUI_TEST(toast_sits_at_bottom_center) {
  ToastEnv env;
  Toast toast;
  toast.setTiming(Toast::Timing{1000, 100});
  toast.show(L"已复制 rgba(86, 120, 30, 255)");
  env.fake.platform().advanceMillis(200);

  const Rect box = toast.bounds(kArea, 96);
  AIUI_CHECK(!box.isEmpty());
  // 水平居中
  AIUI_CHECK(std::abs(box.center().x - kArea.center().x) <= 1);
  // 在客户区内，且靠底
  AIUI_CHECK(box.top > kArea.height() / 2);
  AIUI_CHECK(box.bottom <= kArea.bottom);
  // 圆角必须真的看得出来：高度 33 时应约 7px，不是 1px。
  RecordingBackend backend;
  Painter painter(backend);
  toast.paint(painter, kArea, 96);
  AIUI_CHECK(!backend.fills().empty());
  if (!backend.fills().empty()) {
    const int radius = backend.fills().back().radius;
    AIUI_CHECK(radius >= box.height() / 6);
    AIUI_CHECK(radius < box.height() / 2);
  }
}

// 弹出：一开始在最终位置下方，随后上浮并带过冲；消失时不动
AIUI_TEST(toast_pops_up_from_below) {
  ToastEnv env;
  Toast toast;
  toast.setTiming(Toast::Timing{1000, 100, 200});

  toast.show(L"已复制");
  // 起点：还在下方，所以底边比最终位置更靠下
  const Rect atStart = toast.bounds(kArea, 96);
  AIUI_CHECK(!atStart.isEmpty());

  env.fake.platform().advanceMillis(100);
  const Rect midway = toast.bounds(kArea, 96);
  // 上浮中：比起点高（top 更小）
  AIUI_CHECK(midway.top < atStart.top);

  env.fake.platform().advanceMillis(200);
  const Rect settled = toast.bounds(kArea, 96);
  // 落位后不再变化
  env.fake.platform().advanceMillis(100);
  AIUI_CHECK(toast.bounds(kArea, 96) == settled);
  // 最终位置确实比起点高
  AIUI_CHECK(settled.top < atStart.top);
}

// 脏区必须覆盖整段上浮行程：绘制比标脏晚十几毫秒，按瞬时位置会漏边留残影
AIUI_TEST(toast_dirty_bounds_covers_whole_rise) {
  ToastEnv env;
  Toast toast;
  toast.setTiming(Toast::Timing{1000, 100, 200});
  toast.show(L"已复制");

  const Rect startDirty = toast.dirtyBounds(kArea, 96);
  env.fake.platform().advanceMillis(200);
  const Rect settledDirty = toast.dirtyBounds(kArea, 96);

  // 落位后脏区不变：它不跟随瞬时位置，只由落位框与行程长度决定
  AIUI_CHECK(startDirty == settledDirty);
  // 而且覆盖住起点那一帧的实际位置
  const Rect atStart = toast.bounds(kArea, 96);
  AIUI_CHECK(startDirty.top <= atStart.top);
  AIUI_CHECK(startDirty.bottom >= atStart.bottom);
}

// 长文本不能顶到窗口边：会被压到客户区宽度以内
AIUI_TEST(toast_long_text_stays_inside_window) {
  ToastEnv env;
  Toast toast;
  toast.setTiming(Toast::Timing{1000, 100});
  std::wstring longText(80, L'字');
  toast.show(longText);
  env.fake.platform().advanceMillis(200);

  const Rect box = toast.bounds(kArea, 96);
  AIUI_CHECK(!box.isEmpty());
  AIUI_CHECK(box.width() <= kArea.width());
  AIUI_CHECK(box.left >= 0);
  AIUI_CHECK(box.right <= kArea.right);
}

// 隐藏后画不出任何东西；hide() 立刻生效，不等淡出
AIUI_TEST(toast_hide_is_immediate) {
  ToastEnv env;
  Toast toast;
  toast.show(L"已复制");
  env.fake.platform().advanceMillis(100);
  AIUI_CHECK(!paintText(toast).empty());

  toast.hide();
  AIUI_CHECK(!toast.isVisible());
  AIUI_CHECK(paintText(toast).empty());
  AIUI_CHECK(toast.bounds(kArea, 96).isEmpty());
}

// ─────────────────── 整条刷新链路：消失后不留残影 ───────────────────

// 用户实测的缺陷：提示淡出后留下一圈边缘残影，直到下次缩放才被刷掉。
// 这里走真实的 ViewRoot + FakeWindow，验证「消失那一拍」的失效矩形
// 确实盖住整块提示（包含上浮行程），而不是只盖住最后一帧的瞬时位置。
AIUI_TEST(toast_clears_its_area_when_it_disappears) {
  ScopedFakePlatform scoped;
  fonts.create(96);
  FakePlatform& fake = scoped.platform();

  ViewRoot root;
  AIUI_CHECK(root.attach(nullptr, WindowParams{}));
  auto* window = static_cast<FakeWindow*>(root.platformWindow());
  AIUI_CHECK(window != nullptr);

  FrameLayout content;
  root.setContentView(&content);
  root.layout(Rect{0, 0, 800, 600});

  root.showToast(L"已复制 rgba(86, 120, 30, 255)");
  // 记下整块提示该占的区域（含上浮行程），消失时必须全部刷掉
  const Rect toastArea =
      root.toast().dirtyBounds(window->clientRect(), window->dpi());
  AIUI_CHECK(!toastArea.isEmpty());
  window->clearInvalidRects();

  // 走完淡入 + hold + 淡出：按定时器节奏一拍拍推进
  for (int i = 0; i < 200; ++i) {
    fake.advanceMillis(16);
    root.onDelayTimer();
    if (!root.toast().isVisible()) break;
  }
  AIUI_CHECK(!root.toast().isVisible());

  // 消失那一拍必须标脏，且覆盖整块提示（含上浮行程）
  AIUI_CHECK(!window->invalidRects().empty());
  bool covered = false;
  for (const Rect& rc : window->invalidRects()) {
    if (rc.left <= toastArea.left && rc.right >= toastArea.right &&
        rc.top <= toastArea.top && rc.bottom >= toastArea.bottom) {
      covered = true;
    }
  }
  AIUI_CHECK(covered);

  fonts.destroy();
}
