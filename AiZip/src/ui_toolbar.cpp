// 工具按钮条：装配五个按钮并绑定业务动作
#include "ui_toolbar.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_extract.h"
#include "ui_list.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/button_bar.h"

#include <vector>

namespace ui {
namespace {

AiUI::ButtonBar* g_bar = nullptr;
AiUI::Button* g_btnRefresh = nullptr;
AiUI::Button* g_btnExtractSel = nullptr;
AiUI::Button* g_btnExtractAll = nullptr;
AiUI::Button* g_btnAdd = nullptr;
AiUI::Button* g_btnDelete = nullptr;

AiUI::ButtonSpec MakeSpec(const wchar_t* label, AiUI::IconId icon) {
  AiUI::ButtonSpec spec;
  spec.label = label;
  spec.icon = icon;
  spec.hasIcon = true;
  spec.placement = AiUI::IconPlacement::kLeft;
  spec.style = AiUI::ButtonStyle::kOutline;
  spec.height = 32;
  spec.iconSize = 18;
  spec.padX = 12;
  spec.iconGap = 6;
  spec.radius = 7;
  return spec;
}

void RunRefresh() { AppRefreshView(); }

void RunExtractSel() { ExtractSelection(g_app.mainWnd); }

void RunExtractAll() { ShowExtractDialog(g_app.mainWnd, {}); }

void RunAdd() {
  if (g_app.archiveMode) return;
  std::vector<std::wstring> paths;
  const auto selected = GetSelectedItems();
  if (!selected.empty()) {
    for (int index : selected) {
      if (index >= 0 && index < static_cast<int>(g_app.items.size())) {
        paths.push_back(g_app.items[index].fullPath);
      }
    }
  } else {
    paths.push_back(g_app.curDir);
  }
  ShowCompressDialog(g_app.mainWnd, paths);
}

void RunDelete() { DeleteItems(g_app.mainWnd, GetSelectedItems()); }

}  // namespace

// 由生成的布局构造函数调用：控件已建好，这里只挂回调
void AttachToolBar(AiUI::ButtonBar* bar) {
  if (!bar || g_bar) return;
  g_bar = bar;

  g_btnRefresh = bar->add(MakeSpec(L"刷新", AiUI::IconId::kRefresh), 0);
  if (g_btnRefresh) g_btnRefresh->setOnClick(RunRefresh);

  g_btnExtractSel = bar->add(MakeSpec(L"提取选中", AiUI::IconId::kExtract), 1);
  if (g_btnExtractSel) g_btnExtractSel->setOnClick(RunExtractSel);

  g_btnExtractAll = bar->add(MakeSpec(L"全部解压", AiUI::IconId::kExtractAll), 1);
  if (g_btnExtractAll) g_btnExtractAll->setOnClick(RunExtractAll);

  g_btnAdd = bar->add(MakeSpec(L"添加文件", AiUI::IconId::kAdd), 2);
  if (g_btnAdd) g_btnAdd->setOnClick(RunAdd);

  g_btnDelete = bar->add(MakeSpec(L"删除选中", AiUI::IconId::kTrash), 2);
  if (g_btnDelete) g_btnDelete->setOnClick(RunDelete);

  RefreshToolbarState();
}

void RefreshToolbarState() {
  if (!g_bar) return;
  if (g_btnExtractSel) g_btnExtractSel->setEnabled(CanExtractSelected());
  if (g_btnDelete) g_btnDelete->setEnabled(!GetSelectedItems().empty());
  if (g_btnAdd) g_btnAdd->setEnabled(!g_app.archiveMode);
  if (g_btnRefresh) g_btnRefresh->setEnabled(true);
  if (g_btnExtractAll) g_btnExtractAll->setEnabled(true);
  g_bar->relayout();
}

}  // namespace ui
