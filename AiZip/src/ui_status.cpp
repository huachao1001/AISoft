// 底部状态栏：把应用状态翻译成 StatusFields
#include "ui_status.h"

#include "app.h"
#include "sevenzip_loader.h"
#include "ui_list.h"
#include "util.h"
#include "AiUI/widgets/status_bar.h"

#include <cstdint>

namespace ui {
namespace {

AiUI::StatusBar* g_statusBar = nullptr;

}  // namespace

void AttachStatusBar(AiUI::StatusBar* bar) {
  if (!bar || g_statusBar) return;
  g_statusBar = bar;

  AiUI::StatusStyle style;
  style.background = AiUI::kColorTool;
  bar->setStyle(style);
  RefreshStatusBarData();
}

void RefreshStatusBarData() {
  if (!g_statusBar) return;

  AiUI::StatusFields fields;
  if (g_app.archiveMode) {
    fields.left = g_app.archivePath;
    fields.tag = z7::FormatNameFromExtension(util::FileExtension(g_app.archivePath));
    LARGE_INTEGER size{};
    HANDLE file = ::CreateFileW(g_app.archivePath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      ::GetFileSizeEx(file, &size);
      ::CloseHandle(file);
    }
    fields.size = util::FormatSize(static_cast<uint64_t>(size.QuadPart));
    fields.showHome = true;
  } else {
    fields.left = g_app.curDir;
  }

  size_t files = 0;
  size_t dirs = 0;
  uint64_t total = 0;
  for (int index : VisibleRows()) {
    if (index < 0 || index >= static_cast<int>(g_app.items.size())) continue;
    const Item& item = g_app.items[index];
    if (item.isDir) {
      ++dirs;
    } else {
      ++files;
      total += item.size;
    }
  }

  if (g_app.archiveMode) {
    fields.right = L"压缩包内：总计 " + std::to_wstring(dirs) + L" 个文件夹，" +
                   std::to_wstring(files) + L" 个文件";
  } else if (files > 0 && dirs > 0) {
    fields.right = L"总计 " + std::to_wstring(files) + L" 个文件、" + std::to_wstring(dirs) +
                   L" 个文件夹";
  } else if (dirs > 0) {
    fields.right = L"总计 " + std::to_wstring(dirs) + L" 个文件夹";
  } else if (files > 0) {
    fields.right = L"总计 " + std::to_wstring(files) + L" 个文件";
  } else {
    fields.right = L"没有项目";
  }
  if (!g_app.archiveMode && files > 0) fields.right += L"（" + util::FormatSize(total) + L"）";

  const int selected = static_cast<int>(GetSelectedItems().size());
  if (selected > 0) fields.right += L"，已选中 " + std::to_wstring(selected) + L" 项";

  g_statusBar->setFields(fields);
}

}  // namespace ui
