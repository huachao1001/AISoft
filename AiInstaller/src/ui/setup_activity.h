// 通用安装界面
//
// 这类只做装配：把清单里的文案贴到固定布局上，收集「装到哪、什么范围」，
// 真正动手的是 AiInstaller（解压 → 调配置模块 → 执行命令 → 快捷方式 → 卸载入口）。
// 界面里没有任何应用私有知识，换应用只换清单。
//
// 装的时候是「配置页 → 进度页 → 完成页」三段式，同一个窗口切换可见区域：
//   1. 配置页：范围 + 目录 + 「安装」
//   2. 进度页：进度条 + 当前动作，装完自动进入下一步
//   3. 完成页：「立即打开」复选框 + 正中间「完成」
//
// 卸载副本双击进来走另一条路：「维护页 → 挽留页 → 进度页 → 完成页」：
//   1. 维护页：修复 / 卸载 两张带大图标的选项卡（默认修复）
//   2. 挽留页：选了卸载先劝一次，用户仍坚持才真删
//   3. 之后复用安装的进度页与完成页
// 为什么卸载也要界面：双击 uninstall.exe 的人常常只是觉得程序不对劲，
// 一按就删会逼他重装；给他「修复」这个默认选项比删得更彻底更有用。
// 为什么不用多个窗口：窗口会闪、会跳位置，而且用户看到的是同一个任务。
#pragma once

#include "AiInstaller/app_info.h"
#include "AiInstaller/install.h"
#include "AiInstaller/manifest.h"
#include "AiUI/activity.h"
#include "AiUI/widgets/choice_card.h"
#include "AiUI/widgets/form_controls.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/option_view.h"
#include "AiUI/widgets/text_view.h"

#include <atomic>
#include <functional>
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

  // 维护页用的应用图标（系统图标句柄）。卸载副本没有载荷、解不出品牌
  // 素材，只能拿安装目录里主程序的图标顶上。
  void setInstalledIcon(const AiUI::Image& icon) { installedIcon_ = icon; }

  // 提权后由新进程直接执行，不再要求用户点第二次按钮。
  // 请求里带着模式：安装、修复、卸载走同一条提权通道。
  void setAutoRequest(const std::wstring& dir, Scope scope, RequestMode mode = RequestMode::kInstall);

  // 静默安装：装完不留在完成页等点击，直接退出。
  //
  // 它们是同一个「自动安装」入口，但收尾不同：提权重试后面还坐着一个人，
  // 会自己点「完成」；--silent 是无人值守部署，等下去就是永远不回。
  void setSilent(bool silent) { silent_ = silent; }

  // 静默安装失败时用退出码告知调用方：没有界面可看，退出码是唯一信号。
  int exitCode() const { return exitCode_; }

  // 卸载通道：进维护页（修复 / 卸载），由用户选。
  //
  // installDir 必须是安装目录（也就是卸载副本自己所在目录）：卸载要删的是
  // 那里的文件，而清单就在本程序末尾。
  void setMaintain(const std::wstring& installDir, const Manifest& manifest);

  // 静默卸载：不进界面，直接按卸载执行后退出（供卸载项与自动化使用）
  void setAutoUninstall(const std::wstring& installDir, const Manifest& manifest);

  // 需要立刻结束（如「这不是安装包」）：建好窗口后直接关掉
  void setAbortWith(const std::wstring& message);

 protected:
  void onCreate() override;
  void onDestroy() override;
  bool onCloseRequest() override;

 private:
  // 哪一页在显示
  enum class Stage { kConfig, kProgress, kDone, kMaintain, kKeep };

  // 装配维护页与挽留页：文案来自清单，留空时用界面层兜底
  void setupMaintainPage();

  Scope selectedScope() const;
  std::wstring selectedDir() const;
  void applyScope(Scope scope, bool forceDir);
  void browse();

  void showStage(Stage stage);
  void setProgress(float fraction, const std::wstring& text);

  // 落盘、配置、命令、快捷方式；成功返回空串
  void runInstall();
  void runUninstall();
  void runRepair();

  // 维护页「继续」：按选中的卡进入修复或挽留
  void continueFromMaintain();
  // 工作线程入口：跑完后投递回 UI 线程切到完成页
  void startInstallWork(const InstallRequest& request);
  void startRepairWork(const InstallRequest& request);
  void startUninstallWork(const InstallRequest& request);
  // 三者的公共外壳：清状态、切进度页、起线程
  void startTaskWork(std::function<std::wstring()> task);

  // 进度回调（在工作线程上被调用）：只更新快照并请求刷新
  static void onProgress(float fraction, const wchar_t* stage, void* user);
  void applyProgress();
  // 定时器每帧：取一次快照，装完就停
  void tickOnce();
  // UI 线程上的收尾：按结果切完成页或退回出发的那一页
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

  // ── 维护页 / 挽留页 ──
  AiUI::ChoiceCardGroup* maintainGroup_ = nullptr;

  // 五页的容器，切页时只换可见性
  AiUI::View* configPage_ = nullptr;
  AiUI::View* progressPage_ = nullptr;
  AiUI::View* donePage_ = nullptr;
  AiUI::View* maintainPage_ = nullptr;
  AiUI::View* keepPage_ = nullptr;

  Manifest manifest_;
  AppInfo app_;
  std::wstring logoPath_;
  // 维护页的应用图标：卸载副本没有载荷，只能拿已安装主程序的系统图标。
  AiUI::Image installedIcon_;

  // 本窗口装到了哪里：完成页要用它启动程序
  std::wstring installDir_;

  bool autoInstall_ = false;
  std::wstring autoDir_;
  Scope autoScope_ = Scope::kAllUsers;
  RequestMode autoMode_ = RequestMode::kInstall;

  // 静默安装：装完直接退出，不等「完成」按钮。见 setSilent()。
  bool silent_ = false;
  // 静默失败时留给入口的退出码
  int exitCode_ = 0;
  // 维护通道：双击卸载副本或带 --uninstall 进来
  bool maintain_ = false;
  std::wstring maintainDir_;
  // 静默卸载：不建界面直接删（自动化用）
  bool autoUninstall_ = false;
  std::wstring uninstallDir_;

  // 这一轮工作是什么：决定成功后完成页怎么说、按钮怎么点
  enum class Job { kInstall, kRepair, kUninstall };
  Job job_ = Job::kInstall;

  bool abort_ = false;
  std::wstring abortMessage_;
  // 界面上正在做什么：出错时退回哪一页
  Stage failedStage_ = Stage::kConfig;

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
