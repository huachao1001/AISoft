// 底部状态栏：业务适配层，绘制由 widgets::StatusBar 负责
#include "ui_status.h"

#include "app.h"
#include "sevenzip_loader.h"
#include "ui.h"
#include "ui_list.h"
#include "util.h"
#include "AUI/status_bar.h"

#include <memory>

using namespace AUI;

namespace ui {
namespace {

std::unique_ptr<StatusBar> g_statusBar;

}  // namespace

HWND CreateStatusBar(HWND parent, HINSTANCE inst, int id) {
  if (!g_statusBar) {
    g_statusBar = std::make_unique<StatusBar>();
    g_statusBar->Create(parent, inst, id, L"AIZipStatusBar");
  }
  return g_statusBar->hwnd();
}

void RefreshStatusBarData() {
  if (!g_statusBar || !g_statusBar->hwnd()) return;

  StatusFields f;
  if (g_app.archiveMode) {
    f.left = g_app.archivePath;
    const std::wstring ext = util::FileExtension(g_app.archivePath);
    f.tag = z7::FormatNameFromExtension(ext);
    LARGE_INTEGER li{};
    HANDLE h = ::CreateFileW(g_app.archivePath.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      ::GetFileSizeEx(h, &li);
      ::CloseHandle(h);
    }
    f.size = util::FormatSize(static_cast<uint64_t>(li.QuadPart));
    f.showHome = true;
  } else {
    f.left = g_app.curDir;
  }

  size_t files = 0, dirs = 0;
  uint64_t total = 0;
  for (int idx : VisibleRows()) {
    if (idx < 0 || idx >= static_cast<int>(g_app.items.size())) continue;
    const Item& it = g_app.items[idx];
    if (it.isDir) {
      ++dirs;
    } else {
      ++files;
      total += it.size;
    }
  }

  if (g_app.archiveMode) {
    f.right = L"压缩包内：总计 " + std::to_wstring(dirs) + L" 个文件夹，" +
              std::to_wstring(files) + L" 个文件";
  } else if (files > 0 && dirs > 0) {
    f.right = L"总计 " + std::to_wstring(files) + L" 个文件、" + std::to_wstring(dirs) +
              L" 个文件夹";
  } else if (dirs > 0) {
    f.right = L"总计 " + std::to_wstring(dirs) + L" 个文件夹";
  } else if (files > 0) {
    f.right = L"总计 " + std::to_wstring(files) + L" 个文件";
  } else {
    f.right = L"没有项目";
  }
  if (!g_app.archiveMode && files > 0) f.right += L"（" + util::FormatSize(total) + L"）";

  const int selCount = static_cast<int>(GetSelectedItems().size());
  if (selCount > 0) f.right += L"，已选中 " + std::to_wstring(selCount) + L" 项";

  g_statusBar->SetFields(f);
}

void InvalidateStatusBar() {
  if (g_statusBar && g_statusBar->hwnd()) g_statusBar->Invalidate();
}

}  // namespace ui
