// 主窗口的宿主接入：拖放
//
// 拖放走 AiUI::Platform，应用侧不碰系统消息与窗口句柄。
#include "ui_main.h"

#include "app.h"
#include "dlg_compress.h"
#include "main_activity.h"
#include "util.h"
#include "AiUI/platform.h"
#include "AiUI/window.h"

namespace ui {

void HandleDroppedFiles(const std::vector<std::wstring>& paths) {
  if (paths.empty()) return;
  if (paths.size() == 1 && arc::Archive::IsArchiveFile(paths[0])) {
    AppOpenArchive(paths[0]);
  } else {
    ShowCompressDialog(g_app.mainWnd, paths);
  }
}

void AttachHostHooks() {
  // 拖放交给主窗口自己处理：平台层会在 UI 线程回调
  if (AiUI::Window* win = mainActivity() ? mainActivity()->window() : nullptr) {
    if (AiUI::PlatformWindow* pw = win->platformWindow()) {
      pw->setFileDropHandler([](const std::vector<std::wstring>& paths) {
        HandleDroppedFiles(paths);
      });
    }
  }
}

}  // namespace ui
