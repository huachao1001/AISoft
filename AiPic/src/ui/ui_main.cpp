// 主窗口的宿主接入：拖放与单实例转发
//
// 拖放走 AiUI::Platform，应用侧不碰系统消息与窗口句柄。
#include "ui_main.h"

#include "app.h"
#include "main_activity.h"
#include "util.h"
#include "AiUI/platform.h"
#include "AiUI/window.h"

#include <vector>

namespace ui {

namespace {

// 从一批拖进来的路径里挑第一张图：看图的窗口一次只显示一张，
// 多张一起拖多半是误操作，直接打开用户最先看中的那张比弹框解释更省事。
std::wstring firstImage(const std::vector<std::wstring>& paths) {
  for (const auto& path : paths) {
    if (util::IsImageFile(path) && util::PathExists(path)) return path;
  }
  return {};
}

}  // namespace

void AttachHostHooks() {
  AiUI::Window* win = mainActivity() ? mainActivity()->window() : nullptr;
  if (!win) return;

  // 拖放交给主窗口自己处理：平台层会在 UI 线程回调
  if (AiUI::PlatformWindow* pw = win->platformWindow()) {
    pw->setFileDropHandler([](const std::vector<std::wstring>& paths) {
      const std::wstring path = firstImage(paths);
      if (path.empty()) return;
      if (MainActivity* act = mainActivity()) act->openImage(path);
    });
  }

  // 已有一个窗口时再次双击图片：另一个进程把路径递过来，这里切到该图并前置窗口
  AiUI::platform()->setLaunchPayloadHandler([](const std::wstring& payload) {
    if (payload.empty() || !util::IsImageFile(payload) || !util::PathExists(payload)) return;
    if (MainActivity* act = mainActivity()) act->openImage(payload);
    if (AiUI::Window* window = mainActivity() ? mainActivity()->window() : nullptr) {
      AiUI::platform()->bringToFront(window->handle());
    }
  });
}

}  // namespace ui
