#include "app.h"

#include "dlg_compress.h"
#include "dlg_extract.h"
#include "dlg_input.h"
#include "dlg_progress.h"
#include "main_activity.h"
#include "ui_main.h"
#include "util.h"

#include <map>

App g_app;

// ───────────────────────── 视图 ─────────────────────────

namespace {

std::wstring TypeNameFor(const std::wstring& name, bool isDir) {
  if (isDir) return L"文件夹";
  const std::wstring ext = util::FileExtension(name);
  if (ext == L".zip" || ext == L".7z" || ext == L".rar") return L"压缩文件";
  if (ext.empty()) return L"文件";
  std::wstring upper;
  for (size_t i = 1; i < ext.size(); ++i) upper.push_back(static_cast<wchar_t>(towupper(ext[i])));
  return upper + L" 文件";
}

void LoadDirectoryItems(const std::wstring& dir) {
  g_app.items.clear();
  // 隐藏与系统项不进文件列表：与资源管理器默认观感一致
  for (const auto& entry : AiUI::platform()->listDirectory(dir, true)) {
    Item it;
    it.name = entry.name;
    it.fullPath = entry.fullPath;
    it.isDir = entry.isDir;
    it.size = entry.isDir ? 0 : entry.size;
    it.packSize = it.size;
    it.modifiedMillis = entry.modifiedMillis;
    it.typeName = entry.typeName.empty() ? TypeNameFor(it.name, it.isDir) : entry.typeName;
    g_app.items.push_back(std::move(it));
  }
}

void LoadArchiveItems() {
  g_app.items.clear();
  const auto& entries = g_app.archive.Entries();
  const std::wstring prefix = g_app.innerPath.empty() ? L"" : g_app.innerPath + L"\\";

  // 收集当前层级的直接子项，目录去重
  std::vector<Item> dirItems;
  std::map<std::wstring, size_t> dirIndex;

  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    std::wstring rel = e.path;
    if (!prefix.empty()) {
      if (rel.size() <= prefix.size()) continue;
      if (!AiUI::platform()->isPathPrefix(prefix, rel)) continue;
      rel = rel.substr(prefix.size());
    }
    if (rel.empty()) continue;

    const size_t slash = rel.find_first_of(L"\\/");
    if (slash != std::wstring::npos) {
      // 子目录
      const std::wstring dirName = rel.substr(0, slash);
      auto found = dirIndex.find(dirName);
      if (found == dirIndex.end()) {
        Item d;
        d.name = dirName;
        d.typeName = L"文件夹";
        d.isDir = true;
        dirIndex[dirName] = dirItems.size();
        dirItems.push_back(std::move(d));
      }
      auto& agg = dirItems[dirIndex[dirName]];
      agg.size += e.size;
      if (e.hasMtime) agg.modifiedMillis = e.modifiedMillis;
      continue;
    }

    if (e.isDir) {
      // 空目录条目本身
      auto found = dirIndex.find(rel);
      if (found == dirIndex.end()) {
        Item d;
        d.name = rel;
        d.typeName = L"文件夹";
        d.isDir = true;
        d.modifiedMillis = e.modifiedMillis;
        dirIndex[rel] = dirItems.size();
        dirItems.push_back(std::move(d));
      }
      continue;
    }

    Item it;
    it.name = rel;
    it.typeName = TypeNameFor(rel, false);
    it.size = e.size;
    it.packSize = e.packSize;
    it.crc = e.crc;
    it.hasCrc = e.hasCrc;
    it.modifiedMillis = e.modifiedMillis;
    it.method = e.method;
    it.encrypted = e.encrypted;
    it.entryIndex = static_cast<int>(i);
    g_app.items.push_back(std::move(it));
  }

  // 目录排在前面
  g_app.items.insert(g_app.items.begin(), dirItems.begin(), dirItems.end());
}

// 本程序内的窗口才能当模态 owner；最小化时被 owner 的弹窗会一起隐藏，
// 这种情况下退化为无 owner
AiUI::NativeHandle DialogOwner() {
  AiUI::NativeHandle h = g_app.mainWnd;
  return AiUI::platform()->canOwnModal(h) ? h : nullptr;
}

// 统一的提示框出口：图标与按钮组合在平台层映射到各系统的原生样式
void Info(const std::wstring& text, AiUI::NativeHandle owner = nullptr) {
  AiUI::MessageBoxParams params;
  params.title = L"爱压";
  params.message = text;
  params.icon = AiUI::MessageBoxIcon::kInfo;
  AiUI::platform()->showMessageBox(owner, params);
}

void Warn(const std::wstring& text, AiUI::NativeHandle owner = nullptr) {
  AiUI::MessageBoxParams params;
  params.title = L"爱压";
  params.message = text;
  params.icon = AiUI::MessageBoxIcon::kWarning;
  AiUI::platform()->showMessageBox(owner, params);
}

void Fail(const std::wstring& text, AiUI::NativeHandle owner = nullptr) {
  AiUI::MessageBoxParams params;
  params.title = L"爱压";
  params.message = text;
  params.icon = AiUI::MessageBoxIcon::kError;
  AiUI::platform()->showMessageBox(owner, params);
}

// 打开压缩包；只有内核确实索取口令时才弹输入框。
//
// 普通加密包能正常读目录，口令留到解压阶段由 Extract 的回调再问；
// 这里处理的是「头部加密」（7z -mhe=on）这类连目录都读不出来的情况。
bool OpenArchiveWithPassword(arc::Archive& a, const std::wstring& path,
                             AiUI::NativeHandle owner, arc::OpenResult& r) {
  r = a.Open(path);
  if (r.ok) return true;
  if (!r.needsPassword) return false;  // 损坏或格式不支持：交给调用方报错

  // 需要密码：问一次，密码错了允许重试，最多三次
  std::wstring pwd;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (!ui::PromptText(owner, L"输入密码", L"压缩包已加密，请输入密码：", pwd, true)) {
      r.error.clear();  // 用户取消：不当作错误弹框
      return false;
    }
    r = a.Open(path, pwd);
    if (r.ok) return true;
    if (!r.needsPassword) return false;
  }
  return false;
}

}  // namespace

void AppRefreshView() {
  if (g_app.archiveMode) {
    LoadArchiveItems();
  } else {
    LoadDirectoryItems(g_app.curDir);
  }
  // 退出压缩包模式时也要还原标题
  if (ui::MainActivity* activity = ui::mainActivity()) {
    activity->updateTitle();
    // 根结构变化时才重建树，其余情况只同步选中
    activity->refreshAddressBar();
    activity->refreshFileList();
    activity->refreshStatusBar();
    activity->refreshToolBar();
  }
}

void AppNavigateTo(const std::wstring& dirIn) {
  std::wstring dir = dirIn;
  if (dir.empty()) return;
  // 归一化
  while (dir.size() > 3 && (dir.back() == L'\\')) dir.pop_back();
  if (!util::IsDirectory(dir)) {
    Warn(L"路径不存在：\n" + dir, g_app.mainWnd);
    return;
  }

  if (g_app.archiveMode) {
    g_app.archive.Close();
    g_app.archiveMode = false;
    g_app.innerPath.clear();
    g_app.archivePath.clear();
  }

  g_app.curDir = dir;
  g_app.settings.lastDir = dir;

  // 历史前进
  if (g_app.historyPos < 0 || g_app.historyPos >= static_cast<int>(g_app.history.size()) ||
      !AiUI::platform()->equalsPath(g_app.history[g_app.historyPos], dir)) {
    if (g_app.historyPos + 1 < static_cast<int>(g_app.history.size())) {
      g_app.history.erase(g_app.history.begin() + g_app.historyPos + 1, g_app.history.end());
    }
    g_app.history.push_back(dir);
    g_app.historyPos = static_cast<int>(g_app.history.size()) - 1;
  }

  AppRefreshView();
}

void AppOpenArchive(const std::wstring& path) {
  arc::OpenResult r;
  if (!OpenArchiveWithPassword(g_app.archive, path, DialogOwner(), r)) {
    if (r.error.empty()) return;  // 用户取消
    Fail(L"无法打开压缩包：\n" + path + L"\n\n" + r.error, g_app.mainWnd);
    return;
  }
  g_app.archiveMode = true;
  g_app.archivePath = path;
  g_app.innerPath.clear();

  if (g_app.historyPos + 1 < static_cast<int>(g_app.history.size())) {
    g_app.history.erase(g_app.history.begin() + g_app.historyPos + 1, g_app.history.end());
  }
  g_app.history.push_back(path);
  g_app.historyPos = static_cast<int>(g_app.history.size()) - 1;

  AppRefreshView();
}

bool AppExtractArchive(AiUI::NativeHandle owner, const std::wstring& path) {
  if (!arc::Archive::IsArchiveFile(path) || !util::PathExists(path)) {
    Warn(L"不是可识别的压缩包：\n" + path, owner);
    return false;
  }

  arc::OpenResult r;
  if (!OpenArchiveWithPassword(g_app.archive, path, owner, r)) {
    if (!r.error.empty()) {
      Fail(L"无法打开压缩包：\n" + path + L"\n\n" + r.error, owner);
    }
    return false;
  }
  g_app.archivePath = path;
  // 解压对话框靠 archivePath 推默认目标目录，这里不进入浏览模式
  g_app.archiveMode = false;
  g_app.innerPath.clear();
  ui::ShowExtractDialog(owner);
  return true;
}

// 打开压缩包并把内容解到 targetDir，不弹选项对话框；冲突时逐个询问。
// stripIfTopMatches 非空时：包内唯一顶层目录与它同名就去掉这层，避免 a\a\。
// 返回 false 表示已提示过错误或用户取消。
bool ExtractArchiveInto(const std::wstring& path, const std::wstring& targetDir,
                        const std::wstring& stripIfTopMatches, AiUI::NativeHandle owner) {
  // 用全局 archive：进度框在后台线程跑，局部对象在收尾阶段析构会和
  // 线程里的回调抢生命周期，对话框路径一直用 g_app.archive 也是这个原因。
  g_app.archive.Close();
  arc::OpenResult r;
  if (!OpenArchiveWithPassword(g_app.archive, path, owner, r)) {
    if (!r.error.empty()) {
      Fail(L"无法打开压缩包：\n" + path + L"\n\n" + r.error, owner);
    }
    return false;
  }

  arc::ExtractOptions opts;
  opts.targetDir = targetDir;
  if (!stripIfTopMatches.empty()) {
    const std::wstring topDir = arc::Archive::SingleTopLevelDir(g_app.archive.Entries());
    if (!topDir.empty() && AiUI::platform()->equalsPath(topDir, stripIfTopMatches)) {
      opts.stripTopDir = topDir;
    }
  }

  std::wstring error;
  const bool ok = ui::ShowProgressDialog(owner, L"正在解压", path, [&](const arc::ProgressFn& prog) {
    return g_app.archive.Extract(
        {}, opts, prog,
        [owner](const std::wstring& filePath) -> int {
          // 覆盖询问是模态的，必须回到 UI 线程
          int result = 0;
          ui::RunOnUiThread([&] {
            AiUI::MessageBoxParams params;
            params.title = L"爱压";
            params.message = L"文件已存在：\n\n" + filePath + L"\n\n是否覆盖？";
            params.icon = AiUI::MessageBoxIcon::kWarning;
            params.buttons = AiUI::MessageBoxButtons::kYesNo;
            const auto answer = AiUI::platform()->showMessageBox(owner, params);
            result = answer == AiUI::MessageBoxResult::kYes ? 1 : 0;
          });
          return result;
        },
        [owner](std::wstring& out) -> bool {
          bool accepted = false;
          ui::RunOnUiThread([&] {
            std::wstring pwd;
            accepted = ui::PromptText(owner, L"输入密码", L"压缩包已加密，请输入密码：", pwd, true);
            if (accepted) out = pwd;
          });
          return accepted;
        },
        error);
  });

  if (!ok && !error.empty()) Fail(L"解压失败：\n" + error, owner);
  // 关掉句柄：否则压缩包会一直被占用，用户改不了名也删不掉
  g_app.archive.Close();
  if (ok && g_app.mainWnd) AppRefreshView();
  return ok;
}

// 解压到压缩包所在目录本身：不建同名子目录，包内结构原样落到当前目录。
void AppExtractHere(const std::wstring& path) {
  const AiUI::NativeHandle owner = DialogOwner();
  if (!arc::Archive::IsArchiveFile(path) || !util::PathExists(path)) {
    Warn(L"不是可识别的压缩包：\n" + path, owner);
    return;
  }
  ExtractArchiveInto(path, util::DirName(path), L"", owner);
}

// 解压到以压缩包主名命名的子目录（"a.zip" → 同级 "a\"），目录自动创建。
void AppExtractToFolder(const std::wstring& path) {
  const AiUI::NativeHandle owner = DialogOwner();
  if (!arc::Archive::IsArchiveFile(path) || !util::PathExists(path)) {
    Warn(L"不是可识别的压缩包：\n" + path, owner);
    return;
  }

  const std::wstring parent = util::DirName(path);
  // 目录名取压缩包主名："a.tar.gz" → "a"，与菜单文案同一套规则。
  // 不用 MakeUniquePath：它按扩展名拆分，会把 .001 这类分卷后缀当成扩展名。
  std::wstring stem = util::ArchiveStem(path);
  if (stem.empty()) stem = L"archive";
  const std::wstring target = util::JoinPath(parent, stem);
  if (!util::EnsureDirectory(target)) {
    Fail(L"无法创建目标文件夹：\n" + target, owner);
    return;
  }

  ExtractArchiveInto(path, target, stem, owner);
}

void AppGoUp() {
  if (g_app.archiveMode) {
    if (!g_app.innerPath.empty()) {
      const size_t pos = g_app.innerPath.find_last_of(L'\\');
      g_app.innerPath = (pos == std::wstring::npos) ? L"" : g_app.innerPath.substr(0, pos);
      AppRefreshView();
      return;
    }
    // 压缩包根目录：回到所在文件夹
    AppNavigateTo(util::DirName(g_app.archivePath));
    return;
  }
  const std::wstring parent = util::DirName(g_app.curDir);
  if (!parent.empty() && parent != g_app.curDir) AppNavigateTo(parent);
}

void AppGoBack() {
  if (g_app.historyPos > 0) {
    --g_app.historyPos;
    const std::wstring target = g_app.history[g_app.historyPos];
    if (arc::Archive::IsArchiveFile(target) && util::PathExists(target)) {
      arc::OpenResult r;
      if (OpenArchiveWithPassword(g_app.archive, target, DialogOwner(), r)) {
        g_app.archiveMode = true;
        g_app.archivePath = target;
        g_app.innerPath.clear();
        AppRefreshView();
        return;
      }
    }
    const int save = g_app.historyPos;
    AppNavigateTo(target);
    g_app.historyPos = save;
  }
}

void AppGoForward() {
  if (g_app.historyPos + 1 < static_cast<int>(g_app.history.size())) {
    ++g_app.historyPos;
    const std::wstring target = g_app.history[g_app.historyPos];
    if (arc::Archive::IsArchiveFile(target) && util::PathExists(target)) {
      arc::OpenResult r;
      if (OpenArchiveWithPassword(g_app.archive, target, DialogOwner(), r)) {
        g_app.archiveMode = true;
        g_app.archivePath = target;
        g_app.innerPath.clear();
        AppRefreshView();
        return;
      }
    }
    const int save = g_app.historyPos;
    AppNavigateTo(target);
    g_app.historyPos = save;
  }
}

bool AppCanBack() { return g_app.historyPos > 0; }

bool AppCanForward() {
  return g_app.historyPos + 1 < static_cast<int>(g_app.history.size());
}

// ───────────────────────── 外部启动请求 ─────────────────────────

namespace {

bool EqualsNoCase(const std::wstring& a, const wchar_t* b) {
  return AiUI::platform()->equalsPath(a, b);
}

// 一键压缩：直接用源文件名生成 <名字>.zip，不弹对话框。
// 单个文件用主名；多选时用父目录名——用第一个文件的名字会让用户以为
// 只压了那一个，看不出整批都进去了。
std::wstring QuickZipPath(const std::vector<std::wstring>& sources) {
  if (sources.empty()) return L"";
  const std::wstring& first = sources.front();
  const std::wstring dir = util::DirName(first);

  std::wstring base;
  if (sources.size() > 1) {
    base = util::BaseName(dir);
  } else {
    base = util::IsDirectory(first) ? util::BaseName(first) : util::FileStem(first);
  }
  if (base.empty()) base = L"archive";
  return util::JoinPath(dir, base + L".zip");
}

// 同名时自动让路成 "名字 (2).zip"，不覆盖用户已有的包
std::wstring QuickZipUniquePath(const std::wstring& path) {
  return util::PathExists(path) ? util::MakeUniquePath(path) : path;
}

}  // namespace

LaunchRequest ParseLaunchArgs() {
  LaunchRequest req;
  bool actionGiven = false;
  for (const auto& a : AiUI::platform()->commandLineArgs()) {
    if (EqualsNoCase(a, L"-compress")) {
      req.action = LaunchRequest::Action::kCompress;
      actionGiven = true;
    } else if (EqualsNoCase(a, L"-compress-zip")) {
      req.action = LaunchRequest::Action::kQuickZip;
      actionGiven = true;
    } else if (EqualsNoCase(a, L"-extract-here")) {
      req.action = LaunchRequest::Action::kExtractHere;
      actionGiven = true;
    } else if (EqualsNoCase(a, L"-extract-to-folder")) {
      req.action = LaunchRequest::Action::kExtractToFolder;
      actionGiven = true;
    } else if (EqualsNoCase(a, L"-extract")) {
      req.action = LaunchRequest::Action::kExtract;
      actionGiven = true;
    } else if (!a.empty() && a[0] != L'-') {
      req.paths.push_back(a);
    }
  }
  req.explicitAction = actionGiven;
  // 没有动作开关时按后缀推断，这样资源管理器里「打开方式」选 AIZip 也能用
  if (!actionGiven && !req.paths.empty() && arc::Archive::IsArchiveFile(req.paths[0])) {
    req.action = LaunchRequest::Action::kExtract;
  }
  return req;
}

std::wstring SerializeLaunch(const LaunchRequest& req) {
  std::wstring s = L"-compress";
  if (req.action == LaunchRequest::Action::kExtract) {
    s = L"-extract";
  } else if (req.action == LaunchRequest::Action::kExtractHere) {
    s = L"-extract-here";
  } else if (req.action == LaunchRequest::Action::kExtractToFolder) {
    s = L"-extract-to-folder";
  } else if (req.action == LaunchRequest::Action::kQuickZip) {
    s = L"-compress-zip";
  }
  for (const auto& p : req.paths) {
    s += L'\n';
    s += p;
  }
  return s;
}

LaunchRequest DeserializeLaunch(const std::wstring& packed) {
  LaunchRequest req;
  size_t pos = 0;
  bool actionGiven = false;
  while (pos <= packed.size()) {
    const size_t nl = packed.find(L'\n', pos);
    const std::wstring line =
        packed.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
    pos = (nl == std::wstring::npos) ? packed.size() + 1 : nl + 1;
    if (line.empty()) continue;
    if (EqualsNoCase(line, L"-compress")) {
      req.action = LaunchRequest::Action::kCompress;
      actionGiven = true;
    } else if (EqualsNoCase(line, L"-compress-zip")) {
      req.action = LaunchRequest::Action::kQuickZip;
      actionGiven = true;
    } else if (EqualsNoCase(line, L"-extract-here")) {
      req.action = LaunchRequest::Action::kExtractHere;
      actionGiven = true;
    } else if (EqualsNoCase(line, L"-extract-to-folder")) {
      req.action = LaunchRequest::Action::kExtractToFolder;
      actionGiven = true;
    } else if (EqualsNoCase(line, L"-extract")) {
      req.action = LaunchRequest::Action::kExtract;
      actionGiven = true;
    } else {
      req.paths.push_back(line);
    }
  }
  req.explicitAction = actionGiven;
  if (!actionGiven && !req.paths.empty() && arc::Archive::IsArchiveFile(req.paths[0])) {
    req.action = LaunchRequest::Action::kExtract;
  }
  return req;
}

void AppDispatchLaunch(const LaunchRequest& req) {
  // 浏览模式（无动作开关）：在主窗口里定位到目标路径
  if (!req.explicitAction) {
    if (req.paths.empty()) return;
    const std::wstring& arg = req.paths[0];
    if (util::IsDirectory(arg)) {
      AppNavigateTo(arg);
    } else if (arc::Archive::IsArchiveFile(arg) && util::PathExists(arg)) {
      AppOpenArchive(arg);
    } else {
      AppNavigateTo(util::DirName(util::AbsolutePath(arg)));
    }
    return;
  }

  const AiUI::NativeHandle owner = DialogOwner();

  if (req.action == LaunchRequest::Action::kQuickZip) {
    if (req.paths.empty()) {
      Info(L"没有可压缩的文件。", owner);
      return;
    }
    AppQuickZip(req.paths);
    return;
  }

  if (req.action == LaunchRequest::Action::kCompress) {
    if (req.paths.empty()) {
      Info(L"没有可压缩的文件。", owner);
      return;
    }
    ui::ShowCompressDialog(owner, req.paths);
    return;
  }

  // ── 解压 ──
  if (req.paths.empty()) {
    Info(L"没有指定压缩包。", owner);
    return;
  }
  if (req.action == LaunchRequest::Action::kExtractHere) {
    // 多选时逐个解压到各自目录，不再要求只有一个包
    for (const auto& p : req.paths) AppExtractHere(p);
    return;
  }
  if (req.action == LaunchRequest::Action::kExtractToFolder) {
    for (const auto& p : req.paths) AppExtractToFolder(p);
    return;
  }
  if (req.paths.size() > 1) {
    Info(L"一次只能解压一个压缩包。", owner);
    return;
  }
  const std::wstring& path = req.paths[0];
  AppExtractArchive(owner, path);
}

void AppQuickZip(const std::vector<std::wstring>& paths) {
  const std::wstring target = QuickZipUniquePath(QuickZipPath(paths));
  if (target.empty()) return;

  arc::CompressOptions opts;
  opts.archivePath = target;
  opts.formatName = L"ZIP";
  // 与压缩对话框的默认档位保持一致：快速压缩不经过对话框，也不能沿用旧设置
  opts.level = 5;
  opts.solid = g_app.settings.solid;

  std::wstring err;
  const bool ok =
      ui::ShowProgressDialog(DialogOwner(), L"正在压缩", target, [&](const arc::ProgressFn& prog) {
        return arc::Compress(paths, opts, prog, err);
      });

  if (!ok && !err.empty()) {
    Fail(L"压缩失败：\n" + err, DialogOwner());
  }
  if (ok && g_app.mainWnd) AppRefreshView();
}
