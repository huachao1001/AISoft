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

// 三档：标准（默认）、速度优先、文件最小
const wchar_t* kLevelNames[] = {L"标准", L"速度优先", L"文件最小"};
const int kLevelValues[] = {5, 1, 9};
constexpr int kLevelCount = 3;
constexpr int kLevelDefault = 0;  // 索引，对应「标准」

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
  // 越界时用首项兜底，不要写死某个具体格式：格式表会变，写死就会与界面对不上
  if (formatIndex < 0 || formatIndex >= n) formatIndex = 0;
  return z7::Formats()[formatIndex].ext;
}

std::wstring nameFor(int formatIndex) {
  const int n = z7::FormatCount();
  if (formatIndex < 0 || formatIndex >= n) formatIndex = 0;
  return z7::Formats()[formatIndex].name;
}

// 扩展名替换时要剥掉的归档后缀。
// 不能只依赖格式表：ZIP / 7Z / TAR.BZ2 已不在可创建列表里，但用户仍可能
// 拿一个 xxx.zip 来改格式，不剥就会叠成 xxx.zip.tar.gz。
// 按长度降序排列，保证 .tar.bz2 先于 .bz2 命中。
const wchar_t* kStrippableExts[] = {
    L".tar.bz2", L".tar.gz", L".tar.xz", L".tar.zst", L".tbz2", L".tgz",
    L".txz",     L".zip",    L".7z",     L".rar",     L".gz",   L".bz2",
    L".xz",      L".zst",    L".tar",
};

// 取不含归档扩展名的文件名（.tar.gz 这类两段扩展要整段剥掉）。
std::wstring stripArchiveExt(const std::wstring& path) {
  const std::wstring base = util::BaseName(path);
  for (const wchar_t* ext : kStrippableExts) {
    const std::wstring e = ext;
    if (base.size() > e.size() &&
        AiUI::platform()->equalsPath(base.substr(base.size() - e.size()), e)) {
      return base.substr(0, base.size() - e.size());
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
  // 状态 → 界面：单选项、档位可用性、扩展名联动
  void applyFormat(int formatIndex);
  void syncExtension(int formatIndex);
  void refreshFormatUi();

  void browse();

  std::vector<std::wstring> sources_;

  // applyFormat 会回写单选组，而单选组的 onChange 又调 applyFormat：
  // 置位期间不再响应 onChange，避免来回打转
  bool syncingFormat_ = false;

  CompressTask task_;
  bool hasTask_ = false;

  // 界面当前显示的格式，索引对应 z7::Formats()
  int formatIndex_ = 0;

  AiUI::InputBox* path_ = nullptr;
  AiUI::RadioGroup* formatGroup_ = nullptr;
  AiUI::ComboBox* level_ = nullptr;
  AiUI::InputBox* volume_ = nullptr;
  AiUI::InputBox* passwordBox_ = nullptr;
};

void DlgCompress::onCreate() {
  setEscapeCloses(true);
  setTitle(L"压缩文件");
  // 级别 / 分卷 / 密码挤在一行；底部按钮贴着下边距，窗口比内容多留一档
  setMinSize(660, 270);

  // ── 初始状态：从设置恢复格式、档位、固实 ──
  // 旧设置里可能是已下线的 TAR.BZ2：查不到就退回 ZIP，
  // 不能退到索引 0 之外的值
  int format = z7::FormatIndexByName(g_app.settings.compressFormat.c_str());
  if (format < 0 || format >= z7::FormatCount()) {
    format = z7::FormatIndexByName(L"ZIP");
    if (format < 0) format = 0;
  }
  formatIndex_ = format;

  // 旧设置里的档位值可能已被移除（7 / 3 / 0 等），取不到就退回「标准」
  int levelIndex = kLevelDefault;
  for (int i = 0; i < kLevelCount; ++i) {
    if (kLevelValues[i] == g_app.settings.compressLevel) {
      levelIndex = i;
      break;
    }
  }

  path_ = findView<AiUI::InputBox>("path");
  formatGroup_ = findView<AiUI::RadioGroup>("formatGroup");
  level_ = findView<AiUI::ComboBox>("level");
  volume_ = findView<AiUI::InputBox>("volume");
  passwordBox_ = findView<AiUI::InputBox>("password");

  if (path_) path_->setText(defaultArchivePath(sources_, formatIndex_));

  // 三项格式各占一个单选，索引即 z7::Formats() 的下标
  if (formatGroup_) {
    formatGroup_->setOnChange([this](int index) {
      if (syncingFormat_) return;
      applyFormat(index);
      refreshFormatUi();
    });
  }

  if (level_) {
    std::vector<std::wstring> items;
    for (int i = 0; i < kLevelCount; ++i) items.emplace_back(kLevelNames[i]);
    level_->setItems(items);
    level_->setSelectedIndex(levelIndex);
  }

  applyFormat(formatIndex_);
  refreshFormatUi();

  if (auto* browseButton = findView<AiUI::Button>("btnBrowse")) {
    browseButton->setOnClick([this] { browse(); });
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
  // 单选索引就是格式表下标；未选中（理论上不会发生）时退回第一项
  const int radio = formatGroup_ ? formatGroup_->checkedIndex() : -1;
  return (radio >= 0 && radio < z7::FormatCount()) ? radio : 0;
}

int DlgCompress::selectedLevel() const {
  const int index = level_ ? level_->selectedIndex() : kLevelDefault;
  return (index >= 0 && index < kLevelCount) ? index : kLevelDefault;
}

uint64_t DlgCompress::selectedVolumeSize() const {
  const uint64_t bytes = rawVolumeBytes();
  return bytes >= kMinVolumeBytes ? bytes : 0;
}

// 输入框里的数值换算成字节；空、非法或非正数都返回 0。
// 单位固定 GB：分卷下限 100 MB 以上，用 MB 计的单位数太大、读起来费劲。
uint64_t DlgCompress::rawVolumeBytes() const {
  if (!volume_) return 0;
  const std::wstring text = volume_->text();
  if (text.empty()) return 0;

  // 只接受十进制正数；带单位的写法（"1 GB"）在这里被拒绝，
  // 单位由输入框后的标签承载，混在一起容易算错
  wchar_t* end = nullptr;
  const double value = std::wcstod(text.c_str(), &end);
  if (end == text.c_str() || (end && *end != L'\0') || !(value > 0)) return 0;

  const uint64_t scale = 1024ull * 1024ull * 1024ull;  // GB
  return static_cast<uint64_t>(value * static_cast<double>(scale));
}

void DlgCompress::applyFormat(int formatIndex) {
  if (formatIndex < 0 || formatIndex >= z7::FormatCount()) formatIndex = 0;
  const bool changed = formatIndex != formatIndex_;
  formatIndex_ = formatIndex;

  // 回写界面期间屏蔽单选组的 onChange，避免它又调回 applyFormat
  syncingFormat_ = true;
  if (formatGroup_) formatGroup_->setCheckedIndex(formatIndex);
  syncingFormat_ = false;

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

  // TAR 只有存储档，档位下拉置灰
  if (level_) {
    level_->setEnabled(!info || info->maxLevel > 0);
  }
}

void DlgCompress::browse() {
  const std::wstring initial = defaultArchivePath(sources_, formatIndex_);
  const std::wstring picked = AiUI::platform()->pickSaveFile(
      window()->handle(), L"选择输出路径", util::DirName(initial), util::BaseName(initial),
      extFor(formatIndex_),
      {L"ZIP 压缩文件", L"*.zip", L"7Z 压缩文件", L"*.7z", L"TAR.GZ 压缩文件", L"*.tar.gz",
       L"GZ 文件", L"*.gz", L"TAR 归档", L"*.tar", L"所有文件", L"*.*"});
  if (!picked.empty() && path_) path_->setText(picked);
}

bool DlgCompress::onAccept() {
  std::wstring outPath = path_ ? path_->text() : std::wstring();
  if (outPath.empty()) {
    AiUI::MessageBoxParams params;
    params.title = L"AIZip";
    params.message = L"请填写输出路径。";
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
      params.message = L"分卷大小不能小于 0.1 GB。\n\n留空表示不分卷。";
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

  // GZIP 只收单文件：多文件/目录提前拦下，提示改用 TAR 组合格式
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

  // 记住这次选的格式与档位：「恢复默认」按钮已去掉，确定即视为记住当前选择
  g_app.settings.compressFormat = nameFor(formatIndex);
  g_app.settings.compressLevel = kLevelValues[levelIndex];
  util::SaveSettings(g_app.settings);

  arc::CompressOptions options;
  options.level = kLevelValues[levelIndex];
  options.solid = solid;
  options.password = passwordBox_ ? passwordBox_->text() : std::wstring();
  options.formatName = nameFor(formatIndex);
  options.volumeSize = volumeSize;
  // TAR 只有存储档，内核不认 x 参数
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
  const bool accepted = dialog.runModal(owner, 660, 270);
  if (accepted && dialog.hasTask()) runCompressTask(owner, dialog.task());
}

}  // namespace ui
