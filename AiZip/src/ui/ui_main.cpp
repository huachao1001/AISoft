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

  // 已经在浏览某个压缩包：拖进来的东西就是「加进这个包」。
  // 注意别把「拖另一个压缩包进来」也当成追加——那看着更像要打开它，
  // 用户拖一个包进来的意图是切换目标，不是把一个包塞进另一个包。
  if (g_app.archiveMode && !g_app.homeMode) {
    const bool allArchives =
        paths.size() == 1 && arc::Archive::IsArchiveFile(paths[0]);
    if (!allArchives) {
      const std::wstring error = AppAddToArchive(paths, g_app.innerPath);
      if (!error.empty()) {
        AiUI::MessageBoxParams params;
        params.title = L"爱压";
        params.message = error;
        params.icon = AiUI::MessageBoxIcon::kWarning;
        AiUI::platform()->showMessageBox(g_app.mainWnd, params);
        return;
      }
      // 拖进来只是「记在账上」：真正的重打包要等用户点「导出压缩包」。
      // 这样超大包不会因为拖一个文件就整包解压重压一遍。
      AppRefreshView();
      if (mainActivity()) mainActivity()->refreshToolBar();
      return;
    }
  }

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

  // 同一个包被再次打开：另一个进程把路径递过来，这里切到该包并前置窗口
  AiUI::platform()->setLaunchPayloadHandler([](const std::wstring& payload) {
    if (payload.empty()) return;
    if (arc::Archive::IsArchiveFile(payload) && util::PathExists(payload)) {
      AppOpenArchive(payload);
    } else if (util::IsDirectory(payload)) {
      AppNavigateTo(payload);
    }
    if (AiUI::Window* win = mainActivity() ? mainActivity()->window() : nullptr) {
      AiUI::platform()->bringToFront(win->handle());
    }
  });
}

}  // namespace ui
