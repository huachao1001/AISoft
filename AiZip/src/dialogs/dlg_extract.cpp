// 解压对话框
//
// 只负责收集选项；真正的解压放在进度对话框里跑，因为它可能耗时很久。
#include "dlg_extract.h"

#include "app.h"
#include "archive.h"
#include "dlg_input.h"
#include "dlg_progress.h"
#include "util.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/form_controls.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/option_view.h"
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
  bool openAfter = true;
  bool deleteAfter = false;
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
  void createFolder();
  void editPassword();
  void saveAsDefault();
  // 收集要解压的成员下标；空表示整包
  std::vector<uint32_t> collectIndices() const;

  std::vector<int> selected_;
  std::wstring prefix_;
  std::wstring password_;

  ExtractTask task_;
  bool hasTask_ = false;

  AiUI::InputBox* target_ = nullptr;
  AiUI::RadioGroup* updateGroup_ = nullptr;
  AiUI::RadioGroup* overwriteGroup_ = nullptr;
  AiUI::CheckBox* optOpen_ = nullptr;
  AiUI::CheckBox* optDelete_ = nullptr;
};

void DlgExtract::onCreate() {
  setEscapeCloses(true);
  setTitle(L"解压");
  setMinSize(520, 620);

  target_ = findView<AiUI::InputBox>("target");
  if (target_) target_->setText(defaultTargetDir());

  updateGroup_ = findView<AiUI::RadioGroup>("updateGroup");
  overwriteGroup_ = findView<AiUI::RadioGroup>("overwriteGroup");
  if (overwriteGroup_) {
    // 原版默认「直接覆盖」，这里保持一致
    overwriteGroup_->setCheckedIndex(1);
  }

  optOpen_ = findView<AiUI::CheckBox>("optOpen");
  if (optOpen_) optOpen_->setChecked(g_app.settings.openAfterExtract);
  optDelete_ = findView<AiUI::CheckBox>("optDelete");

  if (auto* browse = findView<AiUI::Button>("btnBrowse")) {
    browse->setOnClick([this] { browseTarget(); });
  }
  if (auto* folder = findView<AiUI::Button>("btnNewFolder")) {
    folder->setOnClick([this] { createFolder(); });
  }
  if (auto* pwd = findView<AiUI::Button>("btnPassword")) {
    pwd->setOnClick([this] { editPassword(); });
  }
  if (auto* save = findView<AiUI::Button>("btnSaveDefault")) {
    save->setOnClick([this] { saveAsDefault(); });
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

void DlgExtract::createFolder() {
  const std::wstring base = target_ ? target_->text() : std::wstring();
  std::wstring name;
  if (!PromptText(window()->handle(), L"新建文件夹", L"文件夹名称：", name)) return;
  if (name.empty()) return;

  const std::wstring path = util::JoinPath(base, name);
  if (!util::EnsureDirectory(path)) {
    AiUI::MessageBoxParams params;
    params.title = L"AIZip";
    params.message = L"无法创建文件夹。";
    params.icon = AiUI::MessageBoxIcon::kError;
    AiUI::platform()->showMessageBox(window()->handle(), params);
    return;
  }
  if (target_) target_->setText(path);
}

void DlgExtract::editPassword() {
  std::wstring pwd = password_;
  if (PromptText(window()->handle(), L"输入密码", L"密码：", pwd, true)) password_ = pwd;
}

void DlgExtract::saveAsDefault() {
  if (target_) g_app.settings.extractDir = target_->text();
  util::SaveSettings(g_app.settings);

  AiUI::MessageBoxParams params;
  params.title = L"AIZip";
  params.message = L"已保存为默认设置。";
  params.icon = AiUI::MessageBoxIcon::kInfo;
  AiUI::platform()->showMessageBox(window()->handle(), params);
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
  task_.options.targetDir = target;
  task_.options.password = password_;
  // 原版「解压并更新」与「解压并替换」都落成 kExtractReplace，只有「仅更新」不同
  task_.options.update = (updateGroup_ && updateGroup_->checkedIndex() == 2)
                            ? arc::ExtractOptions::Update::kNewerOnly
                            : arc::ExtractOptions::Update::kExtractReplace;

  switch (overwriteGroup_ ? overwriteGroup_->checkedIndex() : 1) {
    case 0:
      task_.options.overwrite = arc::ExtractOptions::Overwrite::kAsk;
      break;
    case 2:
      task_.options.overwrite = arc::ExtractOptions::Overwrite::kSkip;
      break;
    case 3:
      task_.options.overwrite = arc::ExtractOptions::Overwrite::kRename;
      break;
    default:
      task_.options.overwrite = arc::ExtractOptions::Overwrite::kOverwrite;
      break;
  }

  task_.openAfter = optOpen_ && optOpen_->isChecked();
  task_.deleteAfter = optDelete_ && optDelete_->isChecked();
  task_.indices = collectIndices();

  g_app.settings.extractDir = target;
  g_app.settings.openAfterExtract = task_.openAfter;
  hasTask_ = true;
  return true;
}

// 对话框已关闭，owner 退回主窗口
void runExtractTask(AiUI::NativeHandle owner, const ExtractTask& task) {
  std::wstring error;
  const bool ok = ShowProgressDialog(owner, L"正在解压", [&](const arc::ProgressFn& prog) {
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

  if (task.deleteAfter && !g_app.archivePath.empty()) {
    const std::wstring archivePath = g_app.archivePath;
    g_app.archive.Close();
    AiUI::platform()->deleteFile(archivePath);
    if (g_app.archiveMode) {
      g_app.archiveMode = false;
      g_app.archivePath.clear();
      AppNavigateTo(util::DirName(archivePath));
    }
  } else if (g_app.mainWnd) {
    AppRefreshView();
  }

  if (task.openAfter) AiUI::platform()->openWithSystem(task.target);
}

}  // namespace

void ShowExtractDialog(AiUI::NativeHandle owner, const std::vector<int>& selected,
                       const std::wstring& prefix) {
  DlgExtract dialog(selected, prefix);
  const bool accepted = dialog.runModal(owner, 560, 660);
  // 解压放到模态循环退出之后：进度框需要自己的模态循环
  if (accepted && dialog.hasTask()) runExtractTask(owner, dialog.task());
}

}  // namespace ui
