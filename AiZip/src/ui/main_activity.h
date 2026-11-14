// 主界面：一个 Activity 持有全部控件与各功能模块
//
// 界面形状由 main_frame.xml 描述；这里只做三件事：装配控件、响应交互、
// 处理窗口级行为（拖放、快捷键、关闭时保存）。数据与规则在
// FileListModel / TreeModel 里，界面文字与业务判断不散落到别处。
#pragma once

#include "AiUI/activity.h"
#include "AiUI/events.h"
#include "AiUI/widgets/list_view.h"
#include "AiUI/widgets/tree_view.h"
#include "file_list_model.h"

#include <memory>
#include <string>
#include <vector>

namespace AiUI {
class BreadcrumbBar;
class Button;
class ButtonBar;
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
  // 在首页 / 浏览页之间切换（由 homeMode 驱动）
  void refreshStage();
  // 把焦点交回文件列表
  void focusFileList();

  // 数据变化后刷新列表 / 状态栏 / 工具栏
  void refreshFileList();
  void refreshStatusBar();
  void refreshToolBar();

  // 搜索框内容变化
  void setFilterText(const std::wstring& text);

 protected:
  const wchar_t* layoutName() const override { return L"main_frame"; }
  void onCreate() override;
  // 主窗口级快捷键（Ctrl+L 编辑地址、Ctrl+F 搜索）
  bool onKeyDown(const AiUI::Key& key, const AiUI::Modifiers& mods) override;
  bool onCloseRequest() override;

 private:
  // ── 装配 ──
  void attachCrumbBar();
  void attachToolBar();
  void attachHome();
  void attachSideTree();
  void attachFileList();
  void attachStatusBar();

  // ── 面包屑 ──
  void refreshCrumbBar();
  void commitAddress(const std::wstring& target);

  // ── 树 ──
  void rebuildSideTree();
  void syncSideTree();
  void onTreeNodeSelected(AiUI::TreeNode* node);
  std::wstring treeActivePath() const;
  bool treeActiveCanExtract() const;

  // ── 列表交互 ──
  void onRowActivated(int row);
  void showListContextMenu();
  void onListKeyDown(AiUI::Key key);
  std::vector<int> selectedItemIndices() const;
  std::vector<std::wstring> selectedPaths() const;

  // ── 文件操作 ──
  bool canExtractSelected() const;
  void extractSelection();
  void deleteItems(const std::vector<int>& indices);
  // 压缩包内：往当前所在目录加文件 / 加文件夹（只登记，导出时才真写回）
  void addFilesToArchive();
  void addFolderToArchive();
  AiUI::NativeHandle nativeWindow() const;

  // 控件由布局创建，这里只借用
  AiUI::BreadcrumbBar* crumb_ = nullptr;
  AiUI::ButtonBar* toolBar_ = nullptr;
  AiUI::TreeView* tree_ = nullptr;
  AiUI::ListView* list_ = nullptr;
  AiUI::StatusBar* status_ = nullptr;

  // 首页与浏览页：按 homeMode 二选一显示
  AiUI::View* homePane_ = nullptr;
  AiUI::View* browsePane_ = nullptr;

  AiUI::Button* btnRefresh_ = nullptr;
  AiUI::Button* btnExtractSel_ = nullptr;
  AiUI::Button* btnExtractAll_ = nullptr;
  AiUI::Button* btnAdd_ = nullptr;
  AiUI::Button* btnAddFolder_ = nullptr;
  AiUI::Button* btnDelete_ = nullptr;
  AiUI::Button* btnExport_ = nullptr;

  FileListModel model_;
  // 适配器被 ListView 持有，这里也要留住，否则下一帧就问不到数据
  std::shared_ptr<AiUI::ListRowSource> adapter_;
  // 树根的结构标识：只有它变了才重建根节点
  std::wstring rootsKey_;
  // 正在改地址栏时抑制树同步，避免输入过程反复重建
  bool syncingTree_ = false;
};

// 进程内唯一的主窗口
MainActivity* mainActivity();

}  // namespace ui
