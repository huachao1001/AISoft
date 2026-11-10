// 面包屑：把应用状态翻译成 BreadcrumbBar 需要的字段，不再继承控件
#pragma once

#include <string>

namespace AiUI {
class BreadcrumbBar;
}

namespace ui {

// 布局建好控件后调用：挂回调、同步一次内容
void AttachCrumbBar(AiUI::BreadcrumbBar* bar);
// 当前视图变化后同步路径段、后退出按钮与搜索提示
void RefreshCrumbBar();
// 进入地址编辑态（Ctrl+L 或点空白）
void BeginAddressEdit();
// 把焦点交给搜索框（Ctrl+F）
void FocusSearchBox();
// 当前是否处于地址编辑态
bool IsAddressEditing();

}  // namespace ui
