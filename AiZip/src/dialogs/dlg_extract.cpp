// 解压对话框
//
// 只收集目标路径；真正的解压放在进度对话框里跑，因为它可能耗时很久。
// 密码不在这里配置：打开压缩包时若发现需要密码，由 OpenArchiveWithPassword
// 当场询问，解压阶段仍缺密码时再由 Extract 的取密码回调补问。
#include "dlg_extract.h"

#include "app.h"
#include "archive.h"
#include "dlg_input.h"
#include "dlg_progress.h"
#include "util.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/text_view.h"
#include "dialog_activity.h"

#include <vector>

namespace ui {
namespace {

std::wstring defaultTargetDir() {
  if (!g_app.archivePath.empty()) {
    const std::wstring dir = util::DirName(g_app.archivePath);
    const std::wstring stem = util::FileStem(g_app.archivePath);
    return util::JoinPath(dir, stem);
  }
  if (!g_app.settings.extractDir.empty()) return g_app.settings.extractDir;
  return g_app.curDir;
}

// 解压任务：对话框关闭后再跑，避免把模态循环嵌套在 onAccept 里
struct ExtractTask {
  std::wstring target;
  // 输入包路径：进度窗口主行要显示「正在解压：包名」
  std::wstring archivePath;
  bool openAfter = true;
  arc::ExtractOptions options;
  std::vector<uint32_t> indices;
};

class DlgExtract : public DialogActivity {
 public:
  DlgExtract(std::vector<int> selected, std::wstring prefix)
      : selected_(std::move(selected)), prefix_(std::move(prefix)) {}

  // 用户确认后要执行的任务；未确认时为空
  const ExtractTask& task() const { return task_; }
  bool hasTask() const { return hasTask_; }

 protected:
  const wchar_t* layoutName() const override { return L"dlg_extract"; }
  void onCreate() override;
  bool onAccept() override;

 private:
  void browseTarget();
  // 收集要解压的成员下标；空表示整包
  std::vector<uint32_t> collectIndices() const;

  std::vector<int> selected_;
  std::wstring prefix_;

  ExtractTask task_;
  bool hasTask_ = false;

  AiUI::InputBox* target_ = nullptr;
};

void DlgExtract::onCreate() {
  setEscapeCloses(true);
  // 标题带上目标包名，多开几个解压窗口时才分得清谁是谁
  const std::wstring name = util::BaseName(g_app.archivePath);
  setTitle(name.empty() ? L"解压" : L"解压 - " + name);
  setMinSize(460, 164);

  target_ = findView<AiUI::InputBox>("target");
  if (target_) target_->setText(defaultTargetDir());

  if (auto* browse = findView<AiUI::Button>("btnBrowse")) {
    browse->setOnClick([this] { browseTarget(); });
  }
  if (auto* ok = findView<AiUI::Button>("btnOk")) {
    ok->setOnClick([this] {
      if (!onAccept()) return;
      setAccepted(true);
      close();
    });
  }
  if (auto* cancel = findView<AiUI::Button>("btnCancel")) {
    cancel->setOnClick([this] { close(); });
  }

  if (target_) target_->setFocus();
}

void DlgExtract::browseTarget() {
  const std::wstring current = target_ ? target_->text() : std::wstring();
  const std::wstring picked = AiUI::platform()->pickFolder(window()->handle(),
                                                           L"选择解压目标文件夹", current);
  if (!picked.empty() && target_) target_->setText(picked);
}

std::vector<uint32_t> DlgExtract::collectIndices() const {
  std::vector<uint32_t> indices;
  // 包内路径：条目的完整路径以它开头即属于该目录
  for (int row : selected_) {
    if (row < 0 || row >= static_cast<int>(g_app.items.size())) continue;
    const Item& item = g_app.items[row];
    const std::wstring inner =
        g_app.innerPath.empty() ? item.name : g_app.innerPath + L"\\" + item.name;
    if (item.isDir) {
      // 目录：带上其下全部成员
      for (size_t i = 0; i < g_app.archive.Entries().size(); ++i) {
        if (AiUI::platform()->isPathPrefix(inner, g_app.archive.Entries()[i].path)) {
          indices.push_back(static_cast<uint32_t>(i));
        }
      }
    } else if (item.entryIndex >= 0) {
      indices.push_back(static_cast<uint32_t>(item.entryIndex));
    }
  }
  if (selected_.empty() && !prefix_.empty()) {
    // 左侧树选中的包内目录：按路径前缀收集
    for (size_t i = 0; i < g_app.archive.Entries().size(); ++i) {
      if (AiUI::platform()->isPathPrefix(prefix_, g_app.archive.Entries()[i].path)) {
        indices.push_back(static_cast<uint32_t>(i));
      }
    }
  }
  return indices;
}

bool DlgExtract::onAccept() {
  std::wstring target = target_ ? target_->text() : std::wstring();
  if (target.empty()) {
    AiUI::MessageBoxParams params;
    params.title = L"AIZip";
    params.message = L"请填写解压目标路径。";
    params.icon = AiUI::MessageBoxIcon::kInfo;
    AiUI::platform()->showMessageBox(window()->handle(), params);
    return false;
  }
  target = util::AbsolutePath(target);
  if (!util::EnsureDirectory(target)) {
    AiUI::MessageBoxParams params;
    params.title = L"AIZip";
    params.message = L"无法创建目标文件夹。";
    params.icon = AiUI::MessageBoxIcon::kError;
    AiUI::platform()->showMessageBox(window()->handle(), params);
    return false;
  }

  task_.target = target;
  task_.archivePath = g_app.archivePath;
  task_.options.targetDir = target;
  // 「解压后打开目标文件夹」选项已移除，按原默认行为固定为打开
  task_.openAfter = true;
  task_.indices = collectIndices();

  g_app.settings.extractDir = target;
  hasTask_ = true;
  return true;
}

// 对话框已关闭，owner 退回主窗口
void runExtractTask(AiUI::NativeHandle owner, const ExtractTask& task) {
  std::wstring error;
  const bool ok = ShowProgressDialog(owner, L"正在解压", task.archivePath,
                                     [&](const arc::ProgressFn& prog) {
    return g_app.archive.Extract(
        task.indices, task.options, prog,
        [owner](const std::wstring& path) -> int {
          // 覆盖询问必须回到 UI 线程：它是模态的
          int result = 1;
          RunOnUiThread([&] {
            AiUI::MessageBoxParams params;
            params.title = L"AIZip";
            params.message = L"文件已存在：\n\n" + path + L"\n\n是否覆盖？";
            params.icon = AiUI::MessageBoxIcon::kWarning;
            params.buttons = AiUI::MessageBoxButtons::kYesNo;
            const auto answer = AiUI::platform()->showMessageBox(owner, params);
            result = answer == AiUI::MessageBoxResult::kYes ? 1 : 0;
          });
          return result;
        },
        [owner](std::wstring& out) -> bool {
          bool accepted = false;
          RunOnUiThread([&] {
            std::wstring pwd;
            accepted = PromptText(owner, L"输入密码", L"压缩包已加密，请输入密码：", pwd, true);
            if (accepted) out = pwd;
          });
          return accepted;
        },
        error);
  });

  if (!ok) {
    if (!error.empty()) {
      AiUI::MessageBoxParams params;
      params.title = L"AIZip";
      params.message = L"解压失败：\n" + error;
      params.icon = AiUI::MessageBoxIcon::kError;
      AiUI::platform()->showMessageBox(owner, params);
    }
    return;
  }

  if (g_app.mainWnd) AppRefreshView();
  if (task.openAfter) AiUI::platform()->openWithSystem(task.target);
}

}  // namespace

void ShowExtractDialog(AiUI::NativeHandle owner, const std::vector<int>& selected,
                       const std::wstring& prefix) {
  DlgExtract dialog(selected, prefix);
  // 内容高度 140：上下内边距 32 + 标签 18 + 输入行 38 + 按钮行 52。
  // 窗口留 24 的余量，否则任何一处度量微调都会吃掉底部按钮。
  const bool accepted = dialog.runModal(owner, 560, 164);
  // 解压放到模态循环退出之后：进度框需要自己的模态循环
  if (accepted && dialog.hasTask()) runExtractTask(owner, dialog.task());
}

}  // namespace ui
