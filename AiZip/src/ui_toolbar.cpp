// 工具按钮条：按钮与布局由 widgets::ButtonBar 负责，这里只装配内容
#include "ui_toolbar.h"

#include "app.h"
#include "dlg_compress.h"
#include "dlg_extract.h"
#include "ui.h"
#include "ui_list.h"
#include "AUI/button_bar.h"
#include "AUI/theme.h"

#include <memory>

using namespace AUI;

namespace ui {
namespace {

std::unique_ptr<ButtonBar> g_bar;
Button* g_btnRefresh = nullptr;
Button* g_btnExtractSel = nullptr;
Button* g_btnExtractAll = nullptr;
Button* g_btnAdd = nullptr;
Button* g_btnDelete = nullptr;

ButtonSpec MakeSpec(const wchar_t* label, IconId icon) {
  ButtonSpec s;
  s.label = label;
  s.icon = icon;
  s.hasIcon = true;
  s.hasText = true;
  s.placement = IconPlacement::kLeft;
  s.style = ButtonStyle::kOutline;
  s.height = 32;
  s.iconSize = 18;
  s.padX = 12;
  s.iconGap = 6;
  s.radius = 7;
  return s;
}

void RunRefresh() { AppRefreshView(); }

void RunExtractSel() { ExtractSelection(g_app.mainWnd); }

void RunExtractAll() { ShowExtractDialog(g_app.mainWnd, {}); }

void RunAdd() {
  if (g_app.archiveMode) return;
  std::vector<std::wstring> paths;
  const auto sel = GetSelectedItems();
  if (!sel.empty()) {
    for (int idx : sel) {
      if (idx >= 0 && idx < static_cast<int>(g_app.items.size()))
        paths.push_back(g_app.items[idx].fullPath);
    }
  } else {
    paths.push_back(g_app.curDir);
  }
  ShowCompressDialog(g_app.mainWnd, paths);
}

void RunDelete() { DeleteItems(g_app.mainWnd, GetSelectedItems()); }

}  // namespace

HWND CreateToolBar(HWND parent, HINSTANCE inst, int id) {
  if (!g_bar) {
    g_bar = std::make_unique<ButtonBar>();
    g_bar->Create(parent, inst, id, L"AIZipToolBar");

    g_btnRefresh = g_bar->Add(MakeSpec(L"刷新", IconId::kRefresh), 0);
    g_btnRefresh->SetHandler(RunRefresh);

    g_btnExtractSel = g_bar->Add(MakeSpec(L"提取选中", IconId::kExtract), 1);
    g_btnExtractSel->SetHandler(RunExtractSel);

    g_btnExtractAll = g_bar->Add(MakeSpec(L"全部解压", IconId::kExtractAll), 1);
    g_btnExtractAll->SetHandler(RunExtractAll);

    g_btnAdd = g_bar->Add(MakeSpec(L"添加文件", IconId::kAdd), 2);
    g_btnAdd->SetHandler(RunAdd);

    g_btnDelete = g_bar->Add(MakeSpec(L"删除选中", IconId::kTrash), 2);
    g_btnDelete->SetHandler(RunDelete);
  } else {
    g_bar->Create(parent, inst, id, L"AIZipToolBar");
  }
  return g_bar->hwnd();
}

void InvalidateToolBar() {
  if (!g_bar || !g_bar->hwnd()) return;
  // 可用性在点按时实时判定；这里只需重排并重绘
  g_bar->Relayout();
  g_btnExtractSel->SetEnabled(CanExtractSelected());
  g_btnDelete->SetEnabled(!GetSelectedItems().empty());
  g_btnAdd->SetEnabled(!g_app.archiveMode);
  g_btnRefresh->SetEnabled(true);
  g_btnExtractAll->SetEnabled(true);
  g_bar->Invalidate();
}

}  // namespace ui
