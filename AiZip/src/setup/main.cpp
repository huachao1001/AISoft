// AIZip 安装程序
//
// 只做两件事：把内嵌的 AiZip.exe 放到目标目录、让它自己完成右键菜单注册。
// 注册逻辑不在这里重复：组件是 AiZip.exe 的内嵌资源，安装器再写一份注册表
// 就等于维护两套同样的事实，迟早不一致。
//
// 所有系统能力都走 AiUI::install.h，本文件不出现任何平台 API。
#include "install_activity.h"

#include "AiUI/app.h"
#include "AiUI/font.h"
#include "AiUI/install.h"
#include "AiUI/platform.h"
#include "AiUI/theme_registry.h"

#include <string>
#include <vector>

namespace {

// 内嵌资源：CMake 把构建出的 AiZip.exe 作为 RCDATA 打进本程序
constexpr int kAiZipResourceId = 101;

// 落地文件名与显示名
constexpr const wchar_t* kExeName = L"AiZip.exe";
constexpr const wchar_t* kDisplayName = L"AIZip";

// 让已落地的程序注册右键菜单后退出。参数名与 AiZip 的 main 约定一致。
constexpr const wchar_t* kRegisterArg = L"--register-shell-menu";

}  // namespace

int AiUI::runApp() {
  AiUI::initHostProcess();
  AiUI::startupPlatform();

  const float dpi = AiUI::platform()->screenDpi();
  AiUI::fonts.create(dpi);
  AiUI::Activity::setDefaultTheme(
      &AiUI::ThemeRegistry::getOr(L"light", AiUI::kLightTheme));

  ui::InstallActivity activity;
  if (!activity.create(nullptr)) {
    AiUI::reportFatalError(L"安装程序窗口创建失败。");
    AiUI::fonts.destroy();
    return 1;
  }

  // 居中；尺寸按布局内容固定，不记忆
  constexpr int kWidth = 560;
  constexpr int kHeight = 320;
  const AiUI::Rect work = AiUI::platform()->workArea();
  const float scale = AiUI::platform()->scaleFactor();
  const int w = static_cast<int>(kWidth * scale);
  const int h = static_cast<int>(kHeight * scale);
  const int x = work.left + ((work.right - work.left) - w) / 2;
  const int y = work.top + ((work.bottom - work.top) - h) / 2;
  activity.setInitialBounds(AiUI::Rect{x, y, x + w, y + h});

  activity.show();
  const int code = AiUI::platform()->runEventLoop();

  AiUI::fonts.destroy();
  AiUI::shutdownPlatform();
  return code;
}
