// 主窗口的宿主接入：拖放与单实例转发
//
// 拖放与进程间转递都走 AiUI::Platform，应用侧不碰系统消息与窗口句柄。
#include "ui_main.h"

#include "app.h"
#include "dlg_compress.h"
#include "main_activity.h"
#include "single_instance.h"
#include "util.h"
#include "AiUI/platform.h"
#include "AiUI/window.h"

namespace ui {
namespace {

// 单实例转发用的应用标识，与 main.cpp 里 claimSingleInstance 的保持一致
constexpr const wchar_t* kInstanceId = inst::kBrowserInstanceId;

// 另一个实例传来的路径：目录就导航过去，压缩包就打开
void applyForwardedPath(const std::wstring& arg) {
  if (arg.empty()) return;
  if (util::IsDirectory(arg)) {
    AppNavigateTo(arg);
  } else if (arc::Archive::IsArchiveFile(arg) && util::PathExists(arg)) {
    AppOpenArchive(arg);
  }
}

}  // namespace

void HandleDroppedFiles(const std::vector<std::wstring>& paths) {
  if (paths.empty()) return;
  if (paths.size() == 1 && arc::Archive::IsArchiveFile(paths[0])) {
    AppOpenArchive(paths[0]);
  } else {
    ShowCompressDialog(g_app.mainWnd, paths);
  }
}

void AttachHostHooks() {
  auto* platform = AiUI::platform();
  // 拖放交给主窗口自己处理：平台层会在 UI 线程回调
  if (AiUI::Window* win = mainActivity() ? mainActivity()->window() : nullptr) {
    if (AiUI::PlatformWindow* pw = win->platformWindow()) {
      pw->setFileDropHandler([](const std::vector<std::wstring>& paths) {
        HandleDroppedFiles(paths);
      });
    }
  }

  // 已被唤醒时执行另一个实例传来的参数
  platform->setLaunchPayloadHandler([](const std::wstring& payload) {
    applyForwardedPath(payload);
  });
}

bool ForwardPathToRunningWindow(const std::wstring& path) {
  // 对端负责恢复并聚焦自己的窗口
  return AiUI::platform()->forwardToRunningInstance(kInstanceId, path);
}

}  // namespace ui
