// 通用安装器入口
//
// 同一个 exe 服务所有应用：它不认识任何应用，只读自己末尾的包。包里的清单
// 决定界面文案、要装哪些文件、装完跑什么命令。因此换应用不需要重新编译
// 安装器，重新打包即可。
//
// 命令行：
//   （无参数）        正常安装
//   --uninstall      卸载（安装目录即本程序所在目录）
//   --elevated-retry 提权后的新进程，按带过来的目录与范围直接装
//   --silent         不建界面，直接装到默认目录（批量部署用）
#include "ui/setup_activity.h"

#include "internal_util.h"

#include "AiInstaller/install.h"
#include "AiInstaller/package.h"
#include "AiUI/app.h"
#include "AiUI/font.h"
#include "AiUI/platform.h"
#include "AiUI/theme_registry.h"

#include <string>
#include <vector>

namespace {

constexpr int kWidth = 560;
constexpr int kHeight = 300;

// 从包里解出 logo 到临时文件：图片只能按路径解码，临时文件在程序退出前删除。
//
// 返回值同时带回路径，由调用方在窗口关闭后删掉。系统清理临时目录的时机
// 不确定（往往要等用户下次登录），不主动删就会一直躺在那里。
std::wstring prepareLogo(const AiInstaller::Package& package, const std::wstring& logo) {
  if (logo.empty()) return {};

  const std::wstring tempDir = AiUI::platform()->tempDir();
  if (tempDir.empty()) return {};

  std::vector<uint8_t> blob;
  if (!package.readBlob(logo, blob) || blob.empty()) return {};

  const size_t dot = logo.find_last_of(L'.');
  const std::wstring ext = dot == std::wstring::npos ? L".png" : logo.substr(dot);
  const std::wstring target = tempDir + L"\\aisetup_logo_" +
                              std::to_wstring(AiUI::platform()->processId()) + ext;

  const std::string bytes(blob.begin(), blob.end());
  if (!AiUI::platform()->writeFile(target, bytes)) return {};
  return target;
}

// 取本程序所在目录。卸载时它就是安装目录：安装器被复制进了安装目录，
// 用户从「应用与功能」拉起的正是它。
std::wstring selfDirectory() {
  const std::wstring self = AiUI::platform()->executablePath();
  const size_t slash = self.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring() : self.substr(0, slash);
}

std::wstring packageErrorMessage(const std::wstring& error) {
  if (!error.empty()) return error;
  // 裸安装器与坏包要分开说：前者是打包漏了，后者是文件损坏
  return L"这个安装程序没有携带任何内容。\n\n"
         L"请确认用的是打包后的安装包，而不是安装器本体。";
}

}  // namespace

int AiUI::runApp() {
  AiUI::initHostProcess();
  AiUI::startupPlatform();

  const int dpi = AiUI::platform()->screenDpi();
  AiUI::fonts.create(dpi);
  AiUI::Activity::setDefaultTheme(
      &AiUI::ThemeRegistry::getOr(L"light", AiUI::kLightTheme));

  // 先拆包：没有包就没有清单，界面也就无从谈起
  AiInstaller::Package package;
  std::wstring packageError;
  const bool packaged = package.openSelf(packageError);

  bool uninstall = false;
  bool silent = false;
  for (const auto& arg : AiUI::platform()->commandLineArgs()) {
    if (arg == L"--uninstall") uninstall = true;
    if (arg == L"--silent") silent = true;
  }

  // 界面放进内层作用域：退出时要先让 Activity 析构，图片句柄才会释放，
  // 否则临时 logo 还被 GDI+ 占着，删不掉。
  int code = 0;
  // 临时 logo 的路径。必须放在作用域外：界面析构之后才能删它。
  std::wstring logoPath;
  {
    AiInstaller::ui::SetupActivity activity;

    // 卸载时不能指望包里还有清单：装进目录的那份副本被裁掉了载荷，
    // 它回答不了「当初装了什么」。清单另存在 installer.manifest 里。
    AiInstaller::Manifest installManifest;
    std::wstring manifestError;
    bool haveManifest = false;
    if (uninstall) {
      haveManifest = AiInstaller::loadManifestFile(
          AiInstaller::detail::joinPath(selfDirectory(),
                                        AiInstaller::Package::manifestFileName()),
          installManifest, manifestError);
    }

    if (uninstall && haveManifest) {
      activity.setManifest(installManifest);
      activity.setAutoUninstall(selfDirectory(), installManifest);
    } else if (!packaged) {
      activity.setManifest(AiInstaller::Manifest{});
      activity.setAbortWith(uninstall && !manifestError.empty()
                                ? manifestError
                                : packageErrorMessage(packageError));
    } else {
      activity.setManifest(package.manifest());
      logoPath = prepareLogo(package, package.manifest().logo);
      activity.setLogoPath(logoPath);

      // 提权后的新实例带 --elevated-retry：按带过来的目录与范围直接安装，
      // 用户只需在 UAC 上点一次「是」。
      AiInstaller::InstallRequest request;
      if (AiInstaller::parseElevatedRequest(AiUI::platform()->commandLineArgs(), request)) {
        activity.setAutoInstall(request.targetDir, request.scope);
      } else if (silent) {
        // 静默安装默认「仅当前用户」：这条路没人能在 UAC 上点「是」，
        // 选「所有用户」会拉起一个永远等不到的授权框，进程就卡死在那里。
        activity.setAutoInstall({}, AiInstaller::Scope::kCurrentUser);
      }
    }

    if (!activity.create(nullptr)) {
      AiUI::reportFatalError(L"安装程序窗口创建失败。");
      AiUI::fonts.destroy();
      return 1;
    }

    // 居中；尺寸按布局内容固定，不记忆
    const AiUI::Rect work = AiUI::platform()->workArea();
    const float scale = AiUI::platform()->scaleFactor();
    const int w = static_cast<int>(static_cast<float>(kWidth) * scale);
    const int h = static_cast<int>(static_cast<float>(kHeight) * scale);
    const int x = work.left + ((work.right - work.left) - w) / 2;
    const int y = work.top + ((work.bottom - work.top) - h) / 2;
    activity.setInitialBounds(AiUI::Rect{x, y, x + w, y + h});

    activity.show();
    code = AiUI::platform()->runEventLoop();
    activity.close();
  }

  // Activity 已析构，但图片本身还在 AiUI 的位图缓存里，GDI+ 仍锁着文件。
  // 必须先让 shutdownPlatform() 释放缓存，再删，否则 DeleteFile 必然失败。
  AiUI::fonts.destroy();
  AiUI::shutdownPlatform();

  if (!logoPath.empty()) AiUI::platform()->deleteFile(logoPath);

  return code;
}
