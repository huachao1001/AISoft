// 压缩对话框
//
// 原来的原生控件消息读写（BM_GETCHECK / CB_GETCURSEL / GetWindowText）
// 全部换成了控件对象，界面状态只有一份来源。
#include "dlg_compress.h"

#include "app.h"
#include "archive.h"
#include "sevenzip_loader.h"
#include "dlg_input.h"
#include "dlg_progress.h"
#include "util.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/form_controls.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/option_view.h"
#include "AiUI/widgets/text_view.h"
#include "dialog_activity.h"

#include <utility>
#include <vector>

namespace ui {
namespace {

const wchar_t* kLevelNames[] = {L"存储", L"最快", L"快速", L"标准", L"最大", L"极限"};
const int kLevelValues[] = {0, 1, 3, 5, 7, 9};
constexpr int kLevelCount = 6;

const wchar_t* kUpdateModes[] = {L"添加并替换文件", L"添加并更新文件", L"仅更新已存在的文件",
                                 L"同步文件夹内容"};

// 压缩任务：对话框关闭后再跑，避免把进度框的模态循环嵌进 onAccept
struct CompressTask {
  std::vector<std::pair<std::wstring, std::vector<std::wstring>>> jobs;
  arc::CompressOptions options;
  std::vector<std::wstring> sources;
  std::wstring outPath;
  bool deleteAfter = false;
  bool testAfter = false;
};

std::wstring extFor(int formatIndex) {
  const int n = z7::FormatCount();
  if (formatIndex < 0 || formatIndex >= n) return L".zip";
  return z7::Formats()[formatIndex].ext;
}

std::wstring nameFor(int formatIndex) {
  const int n = z7::FormatCount();
  if (formatIndex < 0 || formatIndex >= n) return L"ZIP";
  return z7::Formats()[formatIndex].name;
}

// 取不含归档扩展名的文件名（.tar.gz 这类两段扩展要整段剥掉）。
std::wstring stripArchiveExt(const std::wstring& path) {
  const std::wstring base = util::BaseName(path);
  for (int i = 0; i < z7::FormatCount(); ++i) {
    const std::wstring ext = z7::Formats()[i].ext;
    if (base.size() > ext.size() &&
        AiUI::platform()->equalsPath(base.substr(base.size() - ext.size()), ext)) {
      return base.substr(0, base.size() - ext.size());
    }
  }
  const size_t dot = base.find_last_of(L'.');
  return (dot == std::wstring::npos) ? base : base.substr(0, dot);
}

std::wstring defaultArchivePath(const std::vector<std::wstring>& sources, int formatIndex) {
  if (sources.empty()) return L"";
  const std::wstring first = sources.front();
  const std::wstring dir = util::DirName(first);
  std::wstring base = util::IsDirectory(first) ? util::BaseName(first) : util::FileStem(first);
  if (base.empty()) base = L"archive";
  return util::JoinPath(dir, base + extFor(formatIndex));
}

class DlgCompress : public DialogActivity {
 public:
  explicit DlgCompress(std::vector<std::wstring> sources)
      : sources_(std::move(sources)) {}

  const CompressTask& task() const { return task_; }
  bool hasTask() const { return hasTask_; }

 protected:
  const wchar_t* layoutName() const override { return L"dlg_compress"; }
  void onCreate() override;
  bool onAccept() override;

 private:
  // 界面 → 状态
  int selectedFormat() const;
  int selectedLevel() const;
  // 状态 → 界面：单选项、下拉可用性、扩展名联动
  void applyFormat(int formatIndex);
  void syncExtension(int formatIndex);
  void refreshFormatUi();

  void browse();
  void editPassword();
  void saveAsDefault();

  std::vector<std::wstring> sources_;
  std::wstring password_;

  CompressTask task_;
  bool hasTask_ = false;

  // 界面当前显示的格式；下拉框只承载第 3 项及以后
  int formatIndex_ = 0;

  AiUI::InputBox* path_ = nullptr;
  AiUI::RadioGroup* formatGroup_ = nullptr;
  AiUI::ComboBox* otherFormat_ = nullptr;
  AiUI::ComboBox* level_ = nullptr;
  AiUI::ComboBox* updateMode_ = nullptr;
  AiUI::InputBox* volume_ = nullptr;
  AiUI::ComboBox* volumeUnit_ = nullptr;
  AiUI::CheckBox* optSolid_ = nullptr;
  AiUI::CheckBox* optTest_ = nullptr;
  AiUI::CheckBox* optDelete_ = nullptr;
  AiUI::CheckBox* optSeparate_ = nullptr;
};

void DlgCompress::onCreate() {
  setEscapeCloses(true);
  setTitle(L"压缩文件");
  // 左栏固定宽度，右侧分组框要放得下勾选项文字；高度容纳三行 + 底部按钮
  setMinSize(620, 480);

  // ── 初始状态：从设置恢复格式、档位、固实 ──
  int format = z7::FormatIndexByName(g_app.settings.compressFormat.c_str());
  if (format < 0 || format >= z7::FormatCount()) format = 0;
  formatIndex_ = format;

  int levelIndex = 3;
  for (int i = 0; i < kLevelCount; ++i) {
    if (kLevelValues[i] == g_app.settings.compressLevel) {
      levelIndex = i;
      break;
    }
  }

  path_ = findView<AiUI::InputBox>("path");
  formatGroup_ = findView<AiUI::RadioGroup>("formatGroup");
  otherFormat_ = findView<AiUI::ComboBox>("otherFormat");
  level_ = findView<AiUI::ComboBox>("level");
  updateMode_ = findView<AiUI::ComboBox>("updateMode");
  volume_ = findView<AiUI::InputBox>("volume");
  volumeUnit_ = findView<AiUI::ComboBox>("volumeUnit");
  optSolid_ = findView<AiUI::CheckBox>("optSolid");
  optTest_ = findView<AiUI::CheckBox>("optTest");
  optDelete_ = findView<AiUI::CheckBox>("optDelete");
  optSeparate_ = findView<AiUI::CheckBox>("optSeparate");

  if (path_) path_->setText(defaultArchivePath(sources_, formatIndex_));

  // 下拉框的第 3 项起是「其他格式」
  if (otherFormat_) {
    std::vector<std::wstring> items;
    for (int i = 2; i < z7::FormatCount(); ++i) items.emplace_back(z7::Formats()[i].name);
    otherFormat_->setItems(items);
    otherFormat_->setOnChange([this](int index) {
      if (!formatGroup_) return;
      formatGroup_->setCheckedIndex(2);
      applyFormat(2 + index);
    });
  }

  if (formatGroup_) {
    formatGroup_->setOnChange([this](int index) {
      // 第三项的真实格式由下拉框决定
      const int target = index < 2 ? index : 2 + otherFormat_->selectedIndex();
      applyFormat(target);
      refreshFormatUi();
    });
  }

  if (level_) {
    std::vector<std::wstring> items;
    for (int i = 0; i < kLevelCount; ++i) items.emplace_back(kLevelNames[i]);
    level_->setItems(items);
    level_->setSelectedIndex(levelIndex);
  }

  if (updateMode_) {
    std::vector<std::wstring> items;
    for (const wchar_t* mode : kUpdateModes) items.emplace_back(mode);
    updateMode_->setItems(items);
    updateMode_->setSelectedIndex(0);
  }

  if (volumeUnit_) {
    volumeUnit_->setItems({L"MB", L"GB", L"KB"});
    volumeUnit_->setSelectedIndex(0);
  }

  if (optSolid_) {
    optSolid_->setChecked(g_app.settings.solid);
    // 固实只有 7Z 支持，格式变化时随之置灰
    optSolid_->setOnChange([this](bool checked) {
      if (optSolid_->isEnabled()) g_app.settings.solid = checked;
    });
  }

  applyFormat(formatIndex_);
  refreshFormatUi();

  if (auto* browseButton = findView<AiUI::Button>("btnBrowse")) {
    browseButton->setOnClick([this] { browse(); });
  }
  if (auto* pwd = findView<AiUI::Button>("btnPassword")) {
    pwd->setOnClick([this] { editPassword(); });
  }
  if (auto* def = findView<AiUI::Button>("btnDefault")) {
    def->setOnClick([this] { saveAsDefault(); });
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

  if (path_) path_->setFocus();
}

int DlgCompress::selectedFormat() const {
  if (formatGroup_ && formatGroup_->checkedIndex() < 2) return formatGroup_->checkedIndex();
  const int index = otherFormat_ ? otherFormat_->selectedIndex() : 0;
  return 2 + (index < 0 ? 0 : index);
}

int DlgCompress::selectedLevel() const {
  const int index = level_ ? level_->selectedIndex() : 3;
  return (index >= 0 && index < kLevelCount) ? index : 3;
}

void DlgCompress::applyFormat(int formatIndex) {
  if (formatIndex < 0 || formatIndex >= z7::FormatCount()) formatIndex = 0;
  const bool changed = formatIndex != formatIndex_;
  formatIndex_ = formatIndex;

  // 单选项与下拉框互相对齐
  if (formatGroup_) {
    formatGroup_->setCheckedIndex(formatIndex < 2 ? formatIndex : 2);
  }
  if (otherFormat_) {
    otherFormat_->setSelectedIndex(formatIndex >= 2 ? formatIndex - 2 : 0, false);
  }

  if (changed) syncExtension(formatIndex);
}

void DlgCompress::syncExtension(int formatIndex) {
  if (!path_) return;
  const std::wstring current = path_->text();
  if (current.empty()) return;

  // 已有归档扩展名要整段剥掉再拼新的，否则连续切换会叠成 xxx.tar.tar.xz
  const std::wstring stem = stripArchiveExt(current);
  if (stem.empty()) return;
  path_->setText(util::JoinPath(util::DirName(current), stem + extFor(formatIndex)));
}

// 切换格式后同步固实与档位的可用性，以及单位下拉的默认值
void DlgCompress::refreshFormatUi() {
  const int n = z7::FormatCount();
  const bool valid = formatIndex_ >= 0 && formatIndex_ < n;
  const z7::FormatInfo* info = valid ? &z7::Formats()[formatIndex_] : nullptr;

  // 只有 7Z 支持固实；TAR/WIM 只有存储档
  if (optSolid_) {
    const bool capable = info && info->solidCapable;
    optSolid_->setEnabled(capable);
    if (!capable) optSolid_->setChecked(false, false);
  }
  if (level_) {
    level_->setEnabled(!info || info->maxLevel > 0);
  }
}

void DlgCompress::browse() {
  const std::wstring initial = defaultArchivePath(sources_, formatIndex_);
  const std::wstring picked = AiUI::platform()->pickSaveFile(
      window()->handle(), L"选择压缩文件名", util::DirName(initial), util::BaseName(initial),
      extFor(formatIndex_),
      {L"ZIP 压缩文件", L"*.zip", L"7Z 压缩文件", L"*.7z", L"TAR.GZ 压缩文件", L"*.tar.gz",
       L"TAR.BZ2 压缩文件", L"*.tar.bz2", L"TAR.XZ 压缩文件", L"*.tar.xz", L"GZIP 文件", L"*.gz",
       L"BZIP2 文件", L"*.bz2", L"XZ 文件", L"*.xz", L"TAR 归档", L"*.tar", L"WIM 映像", L"*.wim",
       L"所有文件", L"*.*"});
  if (!picked.empty() && path_) path_->setText(picked);
}

void DlgCompress::editPassword() {
  std::wstring pwd = password_;
  if (PromptText(window()->handle(), L"设置密码", L"密码（留空表示不加密）：", pwd, true)) {
    password_ = pwd;
  }
}

void DlgCompress::saveAsDefault() {
  g_app.settings.compressFormat = nameFor(selectedFormat());
  g_app.settings.compressLevel = kLevelValues[selectedLevel()];
  g_app.settings.solid = optSolid_ && optSolid_->isChecked();
  util::SaveSettings(g_app.settings);

  AiUI::MessageBoxParams params;
  params.title = L"AIZip";
  params.message = L"已保存为默认设置。";
  params.icon = AiUI::MessageBoxIcon::kInfo;
  AiUI::platform()->showMessageBox(window()->handle(), params);
}

bool DlgCompress::onAccept() {
  std::wstring outPath = path_ ? path_->text() : std::wstring();
  if (outPath.empty()) {
    AiUI::MessageBoxParams params;
    params.title = L"AIZip";
    params.message = L"请填写压缩文件名。";
    params.icon = AiUI::MessageBoxIcon::kInfo;
    AiUI::platform()->showMessageBox(window()->handle(), params);
    return false;
  }

  outPath = util::AbsolutePath(outPath);
  const std::wstring dir = util::DirName(outPath);
  if (!dir.empty() && !util::EnsureDirectory(dir)) {
    AiUI::MessageBoxParams params;
    params.title = L"AIZip";
    params.message = L"无法创建目标目录。";
    params.icon = AiUI::MessageBoxIcon::kError;
    AiUI::platform()->showMessageBox(window()->handle(), params);
    return false;
  }

  if (util::PathExists(outPath)) {
    AiUI::MessageBoxParams params;
    params.title = L"AIZip";
    params.message = L"文件已存在，是否覆盖？";
    params.icon = AiUI::MessageBoxIcon::kWarning;
    params.buttons = AiUI::MessageBoxButtons::kYesNo;
    if (AiUI::platform()->showMessageBox(window()->handle(), params) !=
        AiUI::MessageBoxResult::kYes) {
      return false;
    }
  }

  const int formatIndex = selectedFormat();
  const int levelIndex = selectedLevel();
  const bool separate = optSeparate_ && optSeparate_->isChecked();
  const z7::FormatInfo& info = z7::Formats()[formatIndex];

  // GZIP/BZIP2/XZ 只收单文件：分离模式天然满足，合并模式多文件/目录要提前拦下
  if (!separate && info.singleFile) {
    std::vector<util::SourceItem> probe;
    util::CollectSources(sources_, probe);
    if (probe.size() != 1) {
      AiUI::MessageBoxParams params;
      params.title = L"AIZip";
      params.message = std::wstring(L"「") + info.name +
                       L"」格式一次只能压缩一个文件。\n\n"
                       L"要打包多个文件或整个文件夹，请改用 TAR 组合格式，"
                       L"或勾选「压缩每个选中项到单独的压缩文件」。";
      params.icon = AiUI::MessageBoxIcon::kInfo;
      AiUI::platform()->showMessageBox(window()->handle(), params);
      return false;
    }
  }

  g_app.settings.compressFormat = nameFor(formatIndex);
  g_app.settings.compressLevel = kLevelValues[levelIndex];
  g_app.settings.solid = optSolid_ && optSolid_->isChecked();

  arc::CompressOptions options;
  options.level = kLevelValues[levelIndex];
  options.solid = optSolid_ && optSolid_->isChecked();
  options.password = password_;
  options.formatName = nameFor(formatIndex);
  // TAR/WIM 只有存储档，内核不认 x 参数
  if (info.maxLevel == 0) options.level = 0;

  // 分离模式：每个源各打一个包
  task_.jobs.clear();
  if (separate) {
    for (const auto& src : sources_) {
      const std::wstring base = util::IsDirectory(src) ? util::BaseName(src) : util::FileStem(src);
      task_.jobs.emplace_back(util::JoinPath(dir, base + extFor(formatIndex)),
                              std::vector<std::wstring>{src});
    }
  } else {
    task_.jobs.emplace_back(outPath, sources_);
  }

  task_.options = options;
  task_.sources = sources_;
  task_.outPath = outPath;
  task_.deleteAfter = optDelete_ && optDelete_->isChecked();
  task_.testAfter = optTest_ && optTest_->isChecked();
  hasTask_ = true;
  return true;
}

// 压缩任务：对话框已经关闭，owner 退回主窗口
void runCompressTask(AiUI::NativeHandle owner, const CompressTask& task) {
  std::wstring error;
  const bool ok = ShowProgressDialog(owner, L"正在压缩", [&](const arc::ProgressFn& prog) {
    for (const auto& job : task.jobs) {
      arc::CompressOptions options = task.options;
      options.archivePath = job.first;
      if (!arc::Compress(job.second, options, prog, error)) return false;
    }
    return true;
  });

  if (!ok) {
    if (!error.empty()) {
      AiUI::MessageBoxParams params;
      params.title = L"AIZip";
      params.message = L"压缩失败：\n" + error;
      params.icon = AiUI::MessageBoxIcon::kError;
      AiUI::platform()->showMessageBox(owner, params);
    }
    return;
  }

  // 压缩后删除源文件；目录走回收站，保持与资源管理器一致
  if (task.deleteAfter) {
    for (const auto& src : task.sources) {
      if (util::IsDirectory(src)) {
        AiUI::platform()->moveToTrash(src);
      } else {
        AiUI::platform()->deleteFile(src);
      }
    }
  }

  if (task.testAfter) {
    arc::Archive check;
    const auto result = check.Open(task.outPath);
    if (!result.ok) {
      AiUI::MessageBoxParams params;
      params.title = L"AIZip";
      params.message = L"压缩包测试失败。";
      params.icon = AiUI::MessageBoxIcon::kWarning;
      AiUI::platform()->showMessageBox(owner, params);
    }
  }
}

}  // namespace

void ShowCompressDialog(AiUI::NativeHandle owner, const std::vector<std::wstring>& sources) {
  if (sources.empty()) {
    AiUI::MessageBoxParams params;
    params.title = L"AIZip";
    params.message = L"没有可压缩的内容。";
    params.icon = AiUI::MessageBoxIcon::kInfo;
    AiUI::platform()->showMessageBox(owner, params);
    return;
  }

  DlgCompress dialog(sources);
  const bool accepted = dialog.runModal(owner, 620, 480);
  if (accepted && dialog.hasTask()) runCompressTask(owner, dialog.task());
}

}  // namespace ui
