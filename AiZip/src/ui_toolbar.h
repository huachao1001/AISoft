// 工具按钮条：装配五个按钮并绑定业务动作
#pragma once

namespace AiUI {
class ButtonBar;
}

namespace ui {

// 布局建好按钮条后调用，挂上五个按钮与它们的回调
void AttachToolBar(AiUI::ButtonBar* bar);
// 按当前视图状态刷新按钮可用性
void RefreshToolbarState();

}  // namespace ui
