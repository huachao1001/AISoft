// 底部状态栏：把应用状态翻译成 StatusFields
#pragma once

namespace AiUI {
class StatusBar;
}

namespace ui {

// 布局建好状态栏后调用
void AttachStatusBar(AiUI::StatusBar* bar);
// 视图或选中项变化后刷新文案
void RefreshStatusBarData();

}  // namespace ui
