// 通用安装界面
//
// 这类只做装配：把清单里的文案贴到固定布局上，收集「装到哪、什么范围」，
// 真正动手的是 AiInstaller（解压 → 调配置模块 → 执行命令 → 快捷方式 → 卸载入口）。
// 界面里没有任何应用私有知识，换应用只换清单。
//
// 三段式流程，同一个窗口切换可见区域：
//   1. 配置页：范围 + 目录 + 「安装」
//   2. 进度页：进度条 + 当前动作，装完自动进入下一步
//   3. 完成页：「立即打开」复选框 + 正中间「完成」
// 为什么不用三个窗口：窗口会闪、会跳位置，而且用户看到的是同一个任务。
#pragma once

#include "AiInstaller/app_info.h"
#include "AiInstaller/install.h"
#include "AiInstaller/manifest.h"
#include "AiUI/activity.h"
#include "AiUI/widgets/form_controls.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/option_view.h"
#include "AiUI/widgets/text_view.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace AiInstaller {
namespace ui {

// 定时器载体：View 才有 postDelayed，Activity 没有。定义在 cpp 里，
// 这里只留指针，避免头文件依赖 AiUI::View 的细节。
class TickView;

class SetupActivity : public AiUI::Activity {
 public:
  const wchar_t* layoutName() const override { return L"dlg_setup"; }

  // 清单来自安装包；界面的文案、图标与安装内容全部由它决定
  void setManifest(const Manifest& manifest);

  // 已解到临时文件的 logo 路径。图片只能按路径解码，解包由入口完成。
  void setLogoPath(const std::wstring& path) { logoPath_ = path; }

  // 提权后由新进程直接执行安装，不再要求用户点第二次「安装」
  void setAutoInstall(const std::wstring& dir, Scope scope);

  // 卸载通道：不建界面，执行后退出
  void setAutoUninstall(const std::wstring& installDir, const Manifest& manifest);

  // 需要立刻结束（如「这不是安装包」）：建好窗口后直接关掉
  void setAbortWith(const std::wstring& message);

 protected:
  void onCreate() override;
  void onDestroy() override;
  bool onCloseRequest() override;

 private:
  // 三页里哪一页在显示
  enum class Stage { kConfig, kProgress, kDone };

  Scope selectedScope() const;
  std::wstring selectedDir() const;
  void applyScope(Scope scope, bool forceDir);
  void browse();

  void showStage(Stage stage);
  void setProgress(float fraction, const std::wstring& text);

  // 落盘、配置、命令、快捷方式；成功返回空串
  std::wstring install();
  void runInstall();
  void runUninstall();

  // 工作线程入口：跑完后投递回 UI 线程切到完成页
  void startInstallWork(const InstallRequest& request);
  // 进度回调（在工作线程上被调用）：只更新快照并请求刷新
  static void onProgress(float fraction, const wchar_t* stage, void* user);
  void applyProgress();
  // 定时器每帧：取一次快照，装完就停
  void tickOnce();
  // UI 线程上的收尾：按结果切完成页或退回配置页
  void onWorkFinished();

  // 完成页「完成」按钮
  void finish();

  void showMessage(const std::wstring& text, bool error);

  // ── 配置页 ──
  AiUI::RadioGroup* scopeGroup_ = nullptr;
  AiUI::InputBox* dirBox_ = nullptr;
  AiUI::TextView* hint_ = nullptr;

  // ── 进度页 ──
  AiUI::ProgressBar* bar_ = nullptr;
  AiUI::TextView* progressText_ = nullptr;

  // ── 完成页 ──
  AiUI::CheckBox* runAfterBox_ = nullptr;
  AiUI::Button* btnFinish_ = nullptr;

  // 三页的容器，切页时只换可见性
  AiUI::View* configPage_ = nullptr;
  AiUI::View* progressPage_ = nullptr;
  AiUI::View* donePage_ = nullptr;

  Manifest manifest_;
  AppInfo app_;
  std::wstring logoPath_;

  // 本窗口装到了哪里：完成页要用它启动程序
  std::wstring installDir_;

  bool autoInstall_ = false;
  std::wstring autoDir_;
  Scope autoScope_ = Scope::kAllUsers;

  bool autoUninstall_ = false;
  std::wstring uninstallDir_;

  bool abort_ = false;
  std::wstring abortMessage_;

  // 安装跑在工作线程上，否则进度条永远画不出来：UI 线程被同步安装占满了。
  std::atomic<bool> working_{false};
  // 工作线程与 UI 线程之间的快照。进度回调可能极密集，用互斥量保护一份
  // 可读状态，UI 线程定时取走即可，不必每条回调都投递窗口消息。
  std::mutex snapshotMutex_;
  float snapshotFraction_ = 0.0f;
  std::wstring snapshotStage_;
  bool snapshotDirty_ = false;
  // 已显示的最大比例：进度条只许前进，换步骤时不能回退
  float shownFraction_ = 0.0f;

  // 安装结果，同样由互斥量保护
  bool resultOk_ = false;
  std::wstring resultError_;
  // 工作线程写、UI 线程读，必须是原子量
  std::atomic<bool> resultReady_{false};

  // 自循环定时器：每帧从工作线程取一次快照
  TickView* tick_ = nullptr;

  // 工作线程可能还在跑，窗口不能先销毁；装完前拒绝关闭
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::thread worker_;
};

}  // namespace ui
}  // namespace AiInstaller
