// 主界面：一个 Activity 持有全部控件
//
// 界面形状由 main_frame.xml 描述，这里只做三件事：装配各功能模块、
// 处理窗口级行为（拖放、快捷键、关闭时保存），以及对外暴露少量入口。
// 具体业务逻辑都在 ui_*.cpp 里，Activity 不参与。
#pragma once

#include "AiUI/activity.h"
#include "AiUI/events.h"

#include <string>

namespace AiUI {
class BreadcrumbBar;
class ButtonBar;
class ListView;
class StatusBar;
class TreeView;
}  // namespace AiUI

namespace ui {

// 主窗口 Activity：XML 布局 + 控件装配
class MainActivity : public AiUI::Activity {
 public:
  MainActivity();
  ~MainActivity() override;

  // 标题随当前压缩包变化
  void updateTitle();
  // 视图状态变化后同步面包屑与树（仅根结构变化时才重建树）
  void refreshAddressBar();
  // 把焦点交回文件列表
  void focusFileList();

 protected:
  const wchar_t* layoutName() const override { return L"main_frame"; }
  void onCreate() override;
  // 主窗口级快捷键（Ctrl+L 编辑地址、Ctrl+F 搜索）
  bool onKeyDown(const AiUI::Key& key, const AiUI::Modifiers& mods) override;
  bool onCloseRequest() override;

 private:
  // 按 XML id 取到控件；控件的具体类型由布局决定
  AiUI::BreadcrumbBar* crumb_ = nullptr;
  AiUI::ButtonBar* toolBar_ = nullptr;
  AiUI::TreeView* tree_ = nullptr;
  AiUI::ListView* list_ = nullptr;
  AiUI::StatusBar* status_ = nullptr;
};

// 进程内唯一的主窗口
MainActivity* mainActivity();

}  // namespace ui
