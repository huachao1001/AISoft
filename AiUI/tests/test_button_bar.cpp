// ButtonBar 紧凑模式：窗口变窄时动作不能溢出到看不见的地方
//
// 这组用例守的是一条实际踩到的问题：工具栏移到底部看图区之后，可用宽度
// 比整窗窄（左侧还有缩略图列表）。若按钮仍按文字宽度排，最小窗口下最后
// 几个会被裁到窗口外——不是变挤，而是彻底点不到。
#include "mock_platform.h"
#include "test_framework.h"

#include "AiUI/font.h"
#include "AiUI/widgets/button_bar.h"

#include <string>

namespace {

using namespace aiui_test;
using namespace AiUI;

struct BarEnv {
  ScopedFakePlatform fake;
  BarEnv() { fonts.create(96); }
  ~BarEnv() { fonts.destroy(); }
};

ButtonSpec makeSpec(const wchar_t* label, IconId icon) {
  ButtonSpec spec;
  spec.label = label;
  spec.hasIcon = true;
  spec.icon = icon;
  spec.placement = IconPlacement::kLeft;
  spec.style = ButtonStyle::kOutline;
  spec.height = 32;
  return spec;
}

}  // namespace

// 宽度足够时按钮带文字，宽度不变
AIUI_TEST(button_bar_keeps_labels_when_room_is_plentiful) {
  BarEnv env;
  ButtonBar bar;
  const int groups[] = {0, 0, 1, 1, 2, 2, 3, 4};
  for (int i = 0; i < 8; ++i) {
    bar.addButton(makeSpec(L"按钮名称", IconId::kZoomIn), groups[i]);
  }
  bar.layout(Rect{0, 0, 1400, 52});

  for (int i = 0; i < bar.buttonCount(); ++i) {
    AIUI_CHECK(!bar.buttonAt(i)->isCompact());
  }
  // 最后一个按钮必须落在条内，否则就是被裁掉了
  AIUI_CHECK(bar.buttonAt(7)->bounds().right <= 1400);
}

// 宽度不够时转紧凑（只留图标），但所有按钮仍要落在条内
AIUI_TEST(button_bar_compacts_and_stays_inside) {
  BarEnv env;
  ButtonBar bar;
  const int groups[] = {0, 0, 1, 1, 2, 2, 3, 4};
  for (int i = 0; i < 8; ++i) {
    bar.addButton(makeSpec(L"按钮名称", IconId::kZoomIn), groups[i]);
  }
  bar.layout(Rect{0, 0, 620, 52});

  for (int i = 0; i < bar.buttonCount(); ++i) {
    AIUI_CHECK(bar.buttonAt(i)->isCompact());
  }
  AIUI_CHECK(bar.buttonAt(7)->bounds().right <= 620);
  // 紧凑后每个按钮至少还要放得下图标
  AIUI_CHECK(bar.buttonAt(0)->bounds().width() > 0);
}

// 没有图标的按钮不能进紧凑模式：文字撤掉就只剩个空方块，认不出也点不下手
AIUI_TEST(button_without_icon_keeps_its_label) {
  BarEnv env;
  ButtonBar bar;
  // 前七个带图标，最后一个是纯文字（「100%」那类）
  for (int i = 0; i < 7; ++i) bar.addButton(makeSpec(L"按钮名称", IconId::kZoomIn), i);
  bar.addButton(makeSpec(L"100%", IconId::kNone), 7);

  bar.layout(Rect{0, 0, 620, 52});
  Button* textOnly = bar.buttonAt(7);
  AIUI_CHECK(!textOnly->isCompact());
  AIUI_CHECK(textOnly->bounds().right <= 620);
}

// 极窄时宁可收紧间距也不能把按钮推出右边界
AIUI_TEST(button_bar_never_overflows_even_when_extremely_narrow) {
  BarEnv env;
  ButtonBar bar;
  for (int i = 0; i < 6; ++i) bar.addButton(makeSpec(L"放大", IconId::kZoomIn), i);
  bar.layout(Rect{0, 0, 220, 52});

  for (int i = 0; i < bar.buttonCount(); ++i) {
    AIUI_CHECK(bar.buttonAt(i)->bounds().left >= 0);
    AIUI_CHECK(bar.buttonAt(i)->bounds().right <= 220);
  }
  // 按钮之间不能互相压住
  for (int i = 1; i < bar.buttonCount(); ++i) {
    AIUI_CHECK(bar.buttonAt(i)->bounds().left >= bar.buttonAt(i - 1)->bounds().right);
  }
}
