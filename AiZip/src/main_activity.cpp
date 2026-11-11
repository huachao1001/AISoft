// 主界面 Activity 实现：装配控件、窗口级行为
#include "main_activity.h"

#include "app.h"
#include "sevenzip_loader.h"
#include "ui_crumb.h"
#include "ui_list.h"
#include "ui_main.h"
#include "ui_status.h"
#include "ui_toolbar.h"
#include "ui_tree.h"
#include "util.h"
#include "AiUI/font.h"
#include "AiUI/window.h"
#include "AiUI/widgets/breadcrumb_bar.h"
#include "AiUI/widgets/button_bar.h"
#include "AiUI/widgets/list_view.h"
#include "AiUI/widgets/status_bar.h"
#include "AiUI/widgets/tree_view.h"

#include <commctrl.h>
#include <shellapi.h>

namespace ui {
namespace {

MainActivity* g_activity = nullptr;

}  // namespace

MainActivity::MainActivity() {
  // 视觉总高与逻辑带一致；窗口尺寸由 main.cpp 恢复
  AiUI::WindowStyle style;
  style.topLevel = true;
  style.resizable = true;
  style.minimizable = true;
  style.maximizable = true;
  style.captionHeight = 0;
  setStyle(style);
  setMinSize(kMinWidth, kMinHeight);
  setTitle(L"AIZip");
  // 主窗口不响应 Esc
  setEscapeCloses(false);
}

MainActivity::~MainActivity() {
  if (g_activity == this) g_activity = nullptr;
}

void MainActivity::onCreate() {
  g_activity = this;

  crumb_ = findView<AiUI::BreadcrumbBar>("crumb");
  toolBar_ = findView<AiUI::ButtonBar>("toolBar");
  tree_ = findView<AiUI::TreeView>("tree");
  list_ = findView<AiUI::ListView>("list");
  status_ = findView<AiUI::StatusBar>("status");

  // 各模块只负责"拿到控件后怎么用"，摆放由 XML 决定
  AttachCrumbBar(crumb_);
  AttachToolBar(toolBar_);
  AttachSideTree(tree_);
  AttachFileList(list_);
  AttachStatusBar(status_);

  // 拖放同时注册在宿主窗口与 ViewRoot 子窗口上：光标实际在后者
  HWND host = static_cast<HWND>(window()->handle());
  g_app.mainWnd = host;
  AttachHostHooks(host);
  if (HWND inner = static_cast<HWND>(contentView() ? contentView()->handle() : nullptr)) {
    if (inner != host) AttachHostHooks(inner);
  }
}

bool MainActivity::onKeyDown(const AiUI::Key& key, const AiUI::Modifiers& mods) {
  if (!mods.ctrl) return false;
  switch (key) {
    case AiUI::Key::kL:
      BeginAddressEdit();
      return true;
    case AiUI::Key::kF:
      FocusSearchBox();
      return true;
    default:
      return false;
  }
}

bool MainActivity::onCloseRequest() {
  // 关闭前保存窗口状态；位置尺寸由平台层处理
  WINDOWPLACEMENT wp{};
  wp.length = sizeof(wp);
  HWND hwnd = g_app.mainWnd;
  if (hwnd && ::GetWindowPlacement(hwnd, &wp)) {
    g_app.settings.winMax = (wp.showCmd == SW_SHOWMAXIMIZED);
    if (!g_app.settings.winMax) {
      g_app.settings.winW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
      g_app.settings.winH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    }
  }
  util::SaveSettings(g_app.settings);
  return true;
}

void MainActivity::updateTitle() {
  const std::wstring title =
      g_app.archiveMode ? util::BaseName(g_app.archivePath) + L" - AIZip" : L"AIZip";
  setTitle(title);
}

void MainActivity::refreshAddressBar() {
  RefreshCrumbBar();
  SyncSideTree();
}

void MainActivity::focusFileList() {
  if (list_) list_->setFocus();
}

MainActivity* mainActivity() { return g_activity; }

}  // namespace ui
