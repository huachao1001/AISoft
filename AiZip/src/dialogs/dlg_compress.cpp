// 压缩对话框
//
// 原来的原生控件消息读写（BM_GETCHECK / CB_GETCURSEL / GetWindowText）
// 全部换成了控件对象，界面状态只有一份来源。
#include "dlg_compress.h"

#include "app.h"
#include "archive.h"
#include "sevenzip_loader.h"
#include "volumes.h"
#include "dlg_progress.h"
#include "util.h"
#include "AiUI/widgets/button.h"
#include "AiUI/widgets/form_controls.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/option_view.h"
#include "AiUI/widgets/text_view.h"
#include "dialog_activity.h"

#include <cmath>
#include <utility>
#include <vector>

namespace ui {
namespace {

const wchar_t* kLevelNames[] = {L"存储", L"最快", L"快速", L"标准", L"最大", L"极限"};
const int kLevelValues[] = {0, 1, 3, 5, 7, 9};
constexpr int kLevelCount = 6;

// 分卷下限：更小的卷没有实用价值，卷数还会爆炸
constexpr uint64_t kMinVolumeBytes = 100ull * 1024ull * 1024ull;

// 压缩任务：对话框关闭后再跑，避免把进度框的模态循环嵌进 onAccept
struct CompressTask {
  std::wstring outPath;
  std::vector<std::wstring> sources;
  arc::CompressOptions options;
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
  // 分卷大小（字节）；输入框为空或低于下限时返回 0（不分卷）
  uint64_t selectedVolumeSize() const;
  // 换算后的原始字节数，不套下限；用于在拒绝时给出提示
  uint64_t rawVolumeBytes() const;
  // 状态 → 界面：单选项、下拉可用性、扩展名联动
  void applyFormat(int formatIndex);
  void syncExtension(int formatIndex);
  void refreshFormatUi();

  void browse();
  void saveAsDefault();

  std::vector<std::wstring> sources_;

  CompressTask task_;
  bool hasTask_ = false;

  // 界面当前显示的格式；下拉框只承载第 3 项及以后
  int formatIndex_ = 0;

  AiUI::InputBox* path_ = nullptr;
  AiUI::RadioGroup* formatGroup_ = nullptr;
  AiUI::ComboBox* otherFormat_ = nullptr;
  AiUI::ComboBox* level_ = nullptr;
  AiUI::InputBox* volume_ = nullptr;
  AiUI::ComboBox* volumeUnit_ = nullptr;
  AiUI::InputBox* passwordBox_ = nullptr;
};

void DlgCompress::onCreate() {
  setEscapeCloses(true);
  setTitle(L"压缩文件");
  // 五行表单 + 底部按钮，留一点弹性空间即可；再高会显得中间空
  setMinSize(520, 300);

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
  volume_ = findView<AiUI::InputBox>("volume");
  volumeUnit_ = findView<AiUI::ComboBox>("volumeUnit");
  passwordBox_ = findView<AiUI::InputBox>("password");

  if (path_) path_->setText(defaultArchivePath(sources_, formatIndex_));

  // 下拉框承载「其他格式」，即格式表第 3 项及以后
  if (otherFormat_) {
    std::vector<std::wstring> items;
    for (int i = 2; i < z7::FormatCount(); ++i) items.emplace_back(z7::Formats()[i].name);
    otherFormat_->setItems(items);
    otherFormat_->setOnChange([this](int index) { applyFormat(2 + index); });
  }

  if (formatGroup_) {
    formatGroup_->setOnChange([this](int index) {
      // 选 ZIP / 7Z 时清空「其他格式」，两处不能同时显示不同格式
      if (index < 2 && otherFormat_) otherFormat_->setSelectedIndex(-1, false);
      applyFormat(index < 2 ? index : 2 + (otherFormat_ ? otherFormat_->selectedIndex() : 0));
    });
  }

  if (level_) {
    std::vector<std::wstring> items;
    for (int i = 0; i < kLevelCount; ++i) items.emplace_back(kLevelNames[i]);
    level_->setItems(items);
    level_->setSelectedIndex(levelIndex);
  }

  if (volumeUnit_) {
    volumeUnit_->setItems({L"MB", L"GB"});
    volumeUnit_->setSelectedIndex(0);
  }

  applyFormat(formatIndex_);
  refreshFormatUi();

  if (auto* browseButton = findView<AiUI::Button>("btnBrowse")) {
    browseButton->setOnClick([this] { browse(); });
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
  // 单选有选中项就用它；都没选说明当前用的是下拉框里的其他格式
  const int radio = formatGroup_ ? formatGroup_->checkedIndex() : -1;
  if (radio >= 0 && radio < 2) return radio;
  const int index = otherFormat_ ? otherFormat_->selectedIndex() : -1;
  // 下拉框也没选（两个单选都未选中、下拉为空）时退回 ZIP
  if (index < 0) return 0;
  const int target = 2 + index;
  return target < z7::FormatCount() ? target : 0;
}

int DlgCompress::selectedLevel() const {
  const int index = level_ ? level_->selectedIndex() : 3;
  return (index >= 0 && index < kLevelCount) ? index : 3;
}

uint64_t DlgCompress::selectedVolumeSize() const {
  const uint64_t bytes = rawVolumeBytes();
  return bytes >= kMinVolumeBytes ? bytes : 0;
}

// 输入框里的数值换算成字节；空、非法或非正数都返回 0
uint64_t DlgCompress::rawVolumeBytes() const {
  if (!volume_) return 0;
  const std::wstring text = volume_->text();
  if (text.empty()) return 0;

  // 只接受十进制正数；带单位的写法（"100 MB"）在这里被拒绝，
  // 单位由右侧下拉框决定，混在一起容易算错
  wchar_t* end = nullptr;
  const double value = std::wcstod(text.c_str(), &end);
  if (end == text.c_str() || (end && *end != L'\0') || !(value > 0)) return 0;

  // 下拉框的顺序是 MB / GB，按索引换算
  const int unit = volumeUnit_ ? volumeUnit_->selectedIndex() : 0;
  const uint64_t scale = unit == 1 ? 1024ull * 1024ull * 1024ull   // GB
                                   : 1024ull * 1024ull;            // MB

  return static_cast<uint64_t>(value * static_cast<double>(scale));
}

void DlgCompress::applyFormat(int formatIndex) {
  if (formatIndex < 0 || formatIndex >= z7::FormatCount()) formatIndex = 0;
  const bool changed = formatIndex != formatIndex_;
  formatIndex_ = formatIndex;

  // ZIP / 7Z 用单选表示；其余格式在下拉框里，此时两个单选都不选中
  if (formatGroup_) {
    if (formatIndex < 2) {
      formatGroup_->setCheckedIndex(formatIndex);
    } else {
      formatGroup_->clearChecked();
    }
  }
  if (otherFormat_) {
    // 选 ZIP / 7Z 时下拉框保持空提示，避免同时显示两个格式
    otherFormat_->setSelectedIndex(formatIndex >= 2 ? formatIndex - 2 : -1, false);
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

// 切换格式后同步档位的可用性
void DlgCompress::refreshFormatUi() {
  const int n = z7::FormatCount();
  const bool valid = formatIndex_ >= 0 && formatIndex_ < n;
  const z7::FormatInfo* info = valid ? &z7::Formats()[formatIndex_] : nullptr;

  // TAR/WIM 只有存储档，档位下拉置灰
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

void DlgCompress::saveAsDefault() {
  g_app.settings.compressFormat = nameFor(selectedFormat());
  g_app.settings.compressLevel = kLevelValues[selectedLevel()];
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

  // 分卷时输出是 xxx.001、xxx.002……，主名文件不会被创建，
  // 覆盖判断要落到首卷上，否则用户看不到「已存在」的提示
  const uint64_t volumeSize = selectedVolumeSize();
  // 填了数字却低于下限：不能静默当成不分卷，否则用户以为分卷了
  if (volumeSize == 0) {
    const uint64_t raw = rawVolumeBytes();
    if (raw > 0 && raw < kMinVolumeBytes) {
      AiUI::MessageBoxParams params;
      params.title = L"AIZip";
      params.message = L"分卷大小不能小于 100 MB。\n\n留空表示不分卷。";
      params.icon = AiUI::MessageBoxIcon::kInfo;
      AiUI::platform()->showMessageBox(window()->handle(), params);
      return false;
    }
  }
  const std::wstring firstVolume = volumeSize > 0 ? arc::VolumePath(outPath, 1) : outPath;
  if (util::PathExists(firstVolume)) {
    AiUI::MessageBoxParams params;
    params.title = L"AIZip";
    params.message = volumeSize > 0 ? L"分卷文件已存在，是否覆盖？" : L"文件已存在，是否覆盖？";
    params.icon = AiUI::MessageBoxIcon::kWarning;
    params.buttons = AiUI::MessageBoxButtons::kYesNo;
    if (AiUI::platform()->showMessageBox(window()->handle(), params) !=
        AiUI::MessageBoxResult::kYes) {
      return false;
    }
  }

  const int formatIndex = selectedFormat();
  const int levelIndex = selectedLevel();
  const z7::FormatInfo& info = z7::Formats()[formatIndex];

  // GZIP/BZIP2/XZ 只收单文件：多文件/目录提前拦下，提示改用 TAR 组合格式
  if (info.singleFile) {
    std::vector<util::SourceItem> probe;
    util::CollectSources(sources_, probe);
    if (probe.size() != 1) {
      AiUI::MessageBoxParams params;
      params.title = L"AIZip";
      params.message = std::wstring(L"「") + info.name +
                       L"」格式一次只能压缩一个文件。\n\n"
                       L"要打包多个文件或整个文件夹，请改用 TAR 组合格式。";
      params.icon = AiUI::MessageBoxIcon::kInfo;
      AiUI::platform()->showMessageBox(window()->handle(), params);
      return false;
    }
  }

  // 固实只有 7Z 支持；界面上不再提供勾选，按保存的设置与格式能力决定
  const bool solid = g_app.settings.solid && info.solidCapable;

  g_app.settings.compressFormat = nameFor(formatIndex);
  g_app.settings.compressLevel = kLevelValues[levelIndex];

  arc::CompressOptions options;
  options.level = kLevelValues[levelIndex];
  options.solid = solid;
  options.password = passwordBox_ ? passwordBox_->text() : std::wstring();
  options.formatName = nameFor(formatIndex);
  options.volumeSize = volumeSize;
  // TAR/WIM 只有存储档，内核不认 x 参数
  if (info.maxLevel == 0) options.level = 0;

  task_.options = options;
  task_.sources = sources_;
  task_.outPath = outPath;
  hasTask_ = true;
  return true;
}

// 压缩任务：对话框已经关闭，owner 退回主窗口
void runCompressTask(AiUI::NativeHandle owner, const CompressTask& task) {
  arc::CompressOptions options = task.options;
  options.archivePath = task.outPath;

  std::wstring error;
  const bool ok = ShowProgressDialog(owner, L"正在压缩", [&](const arc::ProgressFn& prog) {
    return arc::Compress(task.sources, options, prog, error);
  });

  if (!ok && !error.empty()) {
    AiUI::MessageBoxParams params;
    params.title = L"AIZip";
    params.message = L"压缩失败：\n" + error;
    params.icon = AiUI::MessageBoxIcon::kError;
    AiUI::platform()->showMessageBox(owner, params);
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
  const bool accepted = dialog.runModal(owner, 520, 300);
  if (accepted && dialog.hasTask()) runCompressTask(owner, dialog.task());
}

}  // namespace ui
