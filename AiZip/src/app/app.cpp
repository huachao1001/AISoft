#include "app.h"

#include "dlg_compress.h"
#include "dlg_extract.h"
#include "dlg_input.h"
#include "dlg_progress.h"
#include "main_activity.h"
#include "ui_main.h"
#include "util.h"

#include <algorithm>
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

// ── 编辑会话的查询辅助 ──
//
// 列表要在导出前就把增删改反映出来，所以这些判断必须早于 LoadArchiveItems 可用。

// 清单统一按「归一化路径」比较，但存进去的是原始大小写：
// 列表要按原样显示名字，比较时忽略大小写。
bool PathInList(const std::vector<std::wstring>& list, const std::wstring& normalized) {
  for (const auto& v : list) {
    if (App::NormalizeInner(v) == normalized) return true;
  }
  return false;
}

void AddUnique(std::vector<std::wstring>& list, const std::wstring& path) {
  if (path.empty()) return;
  if (PathInList(list, App::NormalizeInner(path))) return;
  list.push_back(path);
}

// 新增清单里是否已有这条
bool InAdded(const std::wstring& normalized) {
  return PathInList(g_app.edits.added, normalized);
}

// 这条路径是否落在某个新增条目（或它下面）
bool UnderAdded(const std::wstring& normalized) {
  for (const auto& a : g_app.edits.added) {
    const std::wstring norm = App::NormalizeInner(a);
    if (norm == normalized) return true;
    if (normalized.size() > norm.size() + 1 && normalized.compare(0, norm.size(), norm) == 0 &&
        normalized[norm.size()] == L'\\') {
      return true;
    }
  }
  return false;
}

// 从新增清单里移除这条及其下级
void EraseFromAdded(const std::wstring& normalized) {
  auto& list = g_app.edits.added;
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&normalized](const std::wstring& a) {
                              const std::wstring norm = App::NormalizeInner(a);
                              return norm == normalized ||
                                     (norm.size() > normalized.size() + 1 &&
                                      norm.compare(0, normalized.size(), normalized) == 0 &&
                                      norm[normalized.size()] == L'\\');
                            }),
             list.end());
}

// 删除清单是否覆盖某个包内路径：同一条，或它是被删目录的下级
bool IsDeletedPath(const std::wstring& normalized) {
  for (const auto& d : g_app.edits.deleted) {
    if (d == normalized) return true;
    if (normalized.size() > d.size() + 1 && normalized.compare(0, d.size(), d) == 0 &&
        normalized[d.size()] == L'\\') {
      return true;
    }
  }
  return false;
}

// 镜像目录里某个包内路径对应的磁盘位置
std::wstring StagePathFor(const std::wstring& innerPath) {
  if (g_app.stageDir.empty()) return {};
  std::wstring rel = innerPath;
  std::replace(rel.begin(), rel.end(), L'/', L'\\');
  return util::JoinPath(g_app.stageDir, rel);
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

  std::vector<Item> dirItems;
  std::vector<Item> fileItems;
  std::map<std::wstring, size_t> dirIndex;   // 小写名 → dirItems 下标
  std::map<std::wstring, size_t> fileIndex;  // 小写名 → fileItems 下标

  auto touchDir = [&](const std::wstring& name) -> size_t {
    const std::wstring key = util::ToLower(name);
    auto found = dirIndex.find(key);
    if (found != dirIndex.end()) return found->second;
    Item d;
    d.name = name;
    d.typeName = L"文件夹";
    d.isDir = true;
    dirIndex[key] = dirItems.size();
    dirItems.push_back(std::move(d));
    return dirItems.size() - 1;
  };

  // 1) 包内原有条目。被删除（含整目录删除）的不再出现：界面要像本地
  //    文件浏览器，删掉的行必须马上消失，而不是等导出后重开才消失。
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    std::wstring rel = e.path;
    if (!prefix.empty()) {
      if (rel.size() <= prefix.size()) continue;
      if (!AiUI::platform()->isPathPrefix(prefix, rel)) continue;
      rel = rel.substr(prefix.size());
    }
    if (rel.empty()) continue;
    if (IsDeletedPath(App::NormalizeInner(e.path))) continue;

    const size_t slash = rel.find_first_of(L"\\/");
    if (slash != std::wstring::npos) {
      // 子目录：只按名字去重，大小随时间聚合
      const size_t at = touchDir(rel.substr(0, slash));
      auto& agg = dirItems[at];
      agg.size += e.size;
      if (e.hasMtime) agg.modifiedMillis = e.modifiedMillis;
      continue;
    }

    if (e.isDir) {
      // 空目录条目本身
      const size_t at = touchDir(rel);
      if (e.hasMtime) dirItems[at].modifiedMillis = e.modifiedMillis;
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
    fileIndex[util::ToLower(rel)] = fileItems.size();
    fileItems.push_back(std::move(it));
  }

  // 2) 镜像里新增的条目：导出前它们只存在于临时目录，列表照样要能看见。
  //
  // 只遍历 edits.added（可能几十条），不去扫镜像目录：镜像里还躺着用户
  // 双击时按需解出来的文件，那些是旧条目，混进来会变成重复行。
  const std::wstring stage = g_app.stageDir;
  if (!stage.empty() && util::PathExists(stage)) {
    for (const auto& innerFull : g_app.edits.added) {
      // rel 是相对当前这一层的路径；disk 要按包内全路径去找镜像里的文件
      std::wstring rel = innerFull;
      if (!prefix.empty()) {
        if (rel.size() <= prefix.size()) continue;
        if (!AiUI::platform()->isPathPrefix(prefix, rel)) continue;
        rel = rel.substr(prefix.size());
      }
      if (rel.empty()) continue;

      const std::wstring disk = util::JoinPath(stage, innerFull);
      if (!util::PathExists(disk)) continue;
      const bool diskIsDir = util::IsDirectory(disk);

      // 还隔着层级：说明属于某个子目录，这一层只该出现那个目录行
      const size_t slash = rel.find_first_of(L"\\/");
      if (slash != std::wstring::npos) {
        const size_t at = touchDir(rel.substr(0, slash));
        if (!diskIsDir) dirItems[at].size += AiUI::platform()->fileSize(disk);
        continue;
      }

      if (diskIsDir) {
        touchDir(rel);
        continue;
      }

      const std::wstring key = util::ToLower(rel);
      Item it;
      it.name = rel;
      it.typeName = TypeNameFor(rel, false);
      it.size = AiUI::platform()->fileSize(disk);
      it.packSize = it.size;
      it.modifiedMillis = AiUI::platform()->modifiedTime(disk);
      // 同名旧条目被新内容覆盖：整行换成镜像里的这一份
      auto found = fileIndex.find(key);
      if (found != fileIndex.end()) {
        fileItems[found->second] = std::move(it);
      } else {
        fileIndex[key] = fileItems.size();
        fileItems.push_back(std::move(it));
      }
    }
  }

  // 3) 已经在镜像里被改过的文件：大小与时间按磁盘上的真实值显示，
  //    否则用户改完了看不出任何变化。只查 modified 清单，不逐个 stat。
  for (const auto& norm : g_app.edits.modified) {
    auto found = fileIndex.find(norm);
    if (found == fileIndex.end()) continue;
    const std::wstring disk = StagePathFor(norm);
    if (disk.empty() || !util::PathExists(disk) || util::IsDirectory(disk)) continue;
    Item& it = fileItems[found->second];
    it.size = AiUI::platform()->fileSize(disk);
    it.packSize = it.size;
    it.modifiedMillis = AiUI::platform()->modifiedTime(disk);
  }

  // 目录排在前面
  g_app.items.reserve(dirItems.size() + fileItems.size());
  g_app.items.insert(g_app.items.end(), dirItems.begin(), dirItems.end());
  g_app.items.insert(g_app.items.end(), fileItems.begin(), fileItems.end());
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
  // 先核实「打开过的文件有没有被外部改过」：列表要显示改动后的真实大小，
  // 工具栏也要据此点亮「导出压缩包」。只 stat opened 清单，成本很小。
  AppScanModifiedEntries();

  // 首页没有内容可载入：跳过列目录，界面只剩提示与两个按钮
  if (g_app.homeMode) {
    g_app.items.clear();
  } else if (g_app.archiveMode) {
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
    activity->refreshStage();
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
    // 离开包之前把未导出的改动问清楚：这些改动只存在于镜像里，
    // 一旦换到别的目录就再也写不回去了。
    if (!AppConfirmDiscardEdits(DialogOwner())) return;
    AppDiscardEdits();
    AppClearNestedArchives();
    g_app.archive.Close();
    g_app.archiveMode = false;
    g_app.innerPath.clear();
    g_app.archivePath.clear();
  }

  g_app.curDir = dir;
  g_app.settings.lastDir = dir;
  // 浏览文件系统同样是「有内容可看」的状态，离开首页
  g_app.homeMode = false;

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
  // 换包前把上一个包未导出的改动问清楚，再清嵌套栈与镜像：
  // 临时文件属于上一个包，留着只会漏在 temp 里。
  if (!AppConfirmDiscardEdits(DialogOwner())) return;
  AppClearNestedArchives();
  AppDiscardEdits();

  arc::OpenResult r;
  if (!OpenArchiveWithPassword(g_app.archive, path, DialogOwner(), r)) {
    if (r.error.empty()) return;  // 用户取消
    Fail(L"无法打开压缩包：\n" + path + L"\n\n" + r.error, g_app.mainWnd);
    return;
  }
  // 换包时旧包的镜像目录也要清：按路径散列命名，不复用会一直堆在 temp 里
  util::RemoveDirectoryTree(g_app.stageDir);
  g_app.stageDir.clear();

  g_app.archiveMode = true;
  g_app.homeMode = false;
  g_app.archivePath = path;
  g_app.innerPath.clear();

  if (g_app.historyPos + 1 < static_cast<int>(g_app.history.size())) {
    g_app.history.erase(g_app.history.begin() + g_app.historyPos + 1, g_app.history.end());
  }
  g_app.history.push_back(path);
  g_app.historyPos = static_cast<int>(g_app.history.size()) - 1;

  AppRefreshView();
}

// ───────────────────── 包内条目落地（嵌套 / 打开 / 追加共用） ─────────────────────

namespace {

// 从路径里取一个安全的文件名（去掉目录与可能害事的字符）
std::wstring SafeFileName(const std::wstring& path) {
  std::wstring name = util::BaseName(path);
  for (wchar_t& c : name) {
    if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
        c == L'<' || c == L'>' || c == L'|') {
      c = L'_';
    }
  }
  return name.empty() ? std::wstring(L"nested.bin") : name;
}

// 某一层压缩包的落地镜像目录。按包路径散列，同一层反复进出复用同一个目录，
// 不同层（含内层那个临时包）各占一个，互不覆盖。
std::wstring StageDirFor(const std::wstring& archivePath) {
  const uint32_t hash = util::StableHash(util::ToLower(archivePath));
  wchar_t suffix[64] = {};
  swprintf_s(suffix, L"aizip_stage_%08X", hash);
  return util::JoinPath(AiUI::platform()->tempDir(), suffix);
}

// 当前层用于落地的目录；还没有就建一个。失败返回空串。
std::wstring EnsureStageDir() {
  if (g_app.stageDir.empty()) g_app.stageDir = StageDirFor(g_app.archivePath);
  if (g_app.stageDir.empty()) return {};
  if (!util::EnsureDirectory(g_app.stageDir)) return {};
  return g_app.stageDir;
}

// 把当前层的某个条目按包内相对路径解到镜像目录，返回落地路径。
//
// 已经在镜像里就直接复用：双击一个文件两次不该解两遍，大文件尤其明显。
std::wstring StageEntry(int entryIndex, const std::wstring& entryPath, std::wstring& error) {
  const std::wstring dir = EnsureStageDir();
  if (dir.empty()) {
    error = L"无法创建临时目录";
    return {};
  }

  // 7-Zip 按包内相对路径落地，分隔符可能是 '/'，也可能被去掉了公共前缀
  std::wstring landed = util::JoinPath(dir, entryPath);
  std::replace(landed.begin(), landed.end(), L'/', L'\\');
  if (util::PathExists(landed)) return landed;

  arc::ExtractOptions opts;
  opts.targetDir = dir;
  const std::vector<uint32_t> indices{static_cast<uint32_t>(entryIndex)};
  if (!g_app.archive.Extract(indices, opts, nullptr, nullptr, nullptr, error)) {
    if (error.empty()) error = L"无法解出该内容";
    return {};
  }

  // 路径形态对不上时退一步：解压层可能自己收拾了目录层级
  if (!util::PathExists(landed)) {
    const std::wstring flat = util::JoinPath(dir, SafeFileName(entryPath));
    if (!util::PathExists(flat)) {
      error = L"解出的内容里找不到这个文件";
      return {};
    }
    return flat;
  }
  return landed;
}

}  // namespace

std::wstring AppEnterNestedArchive(int entryIndex, const std::wstring& entryPath) {
  if (!g_app.archiveMode || entryIndex < 0) return L"当前没有打开压缩包";

  std::wstring error;
  const std::wstring landed = StageEntry(entryIndex, entryPath, error);
  if (landed.empty()) return error;

  // 内层先在自己的临时对象里打开，成功之后才动全局状态：
  // 失败时视图停在原处，不会留下「半个嵌套层」。
  App::NestedLayer next;
  arc::OpenResult r;
  if (!OpenArchiveWithPassword(next.archive, landed, DialogOwner(), r)) {
    return r.error.empty() ? L"无法打开内层压缩包" : r.error;
  }

  // 当前这一层整体入栈（连同它的 Archive 句柄与镜像目录），内层接管全局状态
  App::NestedLayer outer;
  outer.archive = std::move(g_app.archive);
  outer.archivePath = g_app.archivePath;
  outer.innerPath = g_app.innerPath;
  outer.stageDir = g_app.stageDir;
  g_app.nestStack.push_back(std::move(outer));

  g_app.archive = std::move(next.archive);
  g_app.archivePath = landed;
  g_app.innerPath.clear();
  // 内层用新的镜像目录；外层那个（含刚解出来的内层包）留在栈里不动
  g_app.stageDir.clear();

  AppRefreshView();
  return {};
}

bool AppLeaveNestedArchive() {
  if (g_app.nestStack.empty()) return false;

  // 栈顶保存的是「进入本层前」的那一层，退出时把它接回来
  App::NestedLayer outer = std::move(g_app.nestStack.back());
  g_app.nestStack.pop_back();

  // 先关掉本层句柄，再删它解出来的镜像目录：Windows 上句柄还在就删不掉文件
  g_app.archive.Close();
  util::RemoveDirectoryTree(g_app.stageDir);

  g_app.archive = std::move(outer.archive);
  g_app.archivePath = outer.archivePath;
  g_app.innerPath = outer.innerPath;
  // 外层镜像目录要留着：内层这个压缩包本身就在里面，删了会连带下次进入失败
  g_app.stageDir = outer.stageDir;

  AppRefreshView();
  return true;
}

bool AppInNestedArchive() { return !g_app.nestStack.empty(); }

void AppClearNestedArchives() {
  for (auto& layer : g_app.nestStack) {
    layer.archive.Close();
    util::RemoveDirectoryTree(layer.stageDir);
  }
  g_app.nestStack.clear();
  util::RemoveDirectoryTree(g_app.stageDir);
  g_app.stageDir.clear();
}

// ───────────────────── 编辑会话 ─────────────────────

std::wstring App::NormalizeInner(const std::wstring& path) {
  std::wstring out = path;
  std::replace(out.begin(), out.end(), L'/', L'\\');
  while (!out.empty() && out.front() == L'\\') out.erase(out.begin());
  while (!out.empty() && out.back() == L'\\') out.pop_back();
  return util::ToLower(out);
}

bool App::HasPendingEdits() const {
  return !edits.deleted.empty() || !edits.added.empty() || !edits.modified.empty();
}

std::wstring AppAddToArchive(const std::vector<std::wstring>& sources,
                             const std::wstring& innerDir) {
  if (!g_app.archiveMode) return L"当前没有打开压缩包";
  if (sources.empty()) return {};
  if (AppInNestedArchive()) {
    return L"包内的压缩包是预览用的临时副本，改动无法保存。\n请先返回上一层再添加文件。";
  }

  const std::wstring dir = EnsureStageDir();
  if (dir.empty()) return L"无法创建临时目录";

  // 新文件只落到镜像里，包里原有内容一律不动：导出时再从原包直接解出来。
  // 这就是打开大包不必先解压的原因。
  const std::wstring relDir = innerDir;
  const std::wstring destDir = relDir.empty() ? dir : util::JoinPath(dir, relDir);
  if (!util::EnsureDirectory(destDir)) return L"无法创建目标文件夹";

  for (const std::wstring& src : sources) {
    if (!util::PathExists(src)) return L"找不到文件：" + src;

    std::wstring landed;
    if (!util::CopyTreeInto(src, destDir, landed)) return L"无法读入：" + util::BaseName(src);

    // 登记每一个新条目（文件与目录都要）：空目录不进清单就会被丢掉。
    // 用落地路径反推包内路径，保留原始大小写——列表要按它显示名字。
    std::vector<util::SourceItem> collected;
    util::CollectSources({landed}, collected);
    for (const auto& item : collected) {
      const std::wstring innerPath = util::JoinPath(relDir, item.relPath);
      const std::wstring norm = App::NormalizeInner(innerPath);
      AddUnique(g_app.edits.added, innerPath);
      // 同名的旧条目如果原本在删除清单里，要撤销，否则新的会被一起删掉
      g_app.edits.deleted.erase(
          std::remove(g_app.edits.deleted.begin(), g_app.edits.deleted.end(), norm),
          g_app.edits.deleted.end());
    }
  }
  return {};
}

std::wstring AppDeleteFromArchive(const std::vector<std::wstring>& innerPaths) {
  if (!g_app.archiveMode) return L"当前没有打开压缩包";
  if (AppInNestedArchive()) {
    return L"包内的压缩包是预览用的临时副本，改动无法保存。\n请先返回上一层再删除。";
  }

  for (const std::wstring& raw : innerPaths) {
    const std::wstring norm = App::NormalizeInner(raw);
    if (norm.empty()) continue;

    // 新增的东西直接忘掉即可，不必登记删除
    const bool wasAdded = UnderAdded(norm);
    EraseFromAdded(norm);

    // 镜像里可能已经解出来过，一并清掉（RemoveDirectoryTree 只认目录）
    const std::wstring disk = StagePathFor(norm);
    if (!disk.empty() && util::PathExists(disk)) {
      if (util::IsDirectory(disk)) {
        util::RemoveDirectoryTree(disk);
      } else {
        AiUI::platform()->deleteFile(disk);
      }
    }
    // 已打开/已修改清单里也不用再留
    g_app.edits.modified.erase(
        std::remove_if(g_app.edits.modified.begin(), g_app.edits.modified.end(),
                       [&norm](const std::wstring& v) {
                         return v == norm || (v.size() > norm.size() + 1 &&
                                              v.compare(0, norm.size(), norm) == 0 &&
                                              v[norm.size()] == L'\\');
                       }),
        g_app.edits.modified.end());
    g_app.edits.opened.erase(
        std::remove_if(g_app.edits.opened.begin(), g_app.edits.opened.end(),
                       [&norm](const App::EditSession::OpenedFile& o) {
                         return o.inner == norm ||
                                (o.inner.size() > norm.size() + 1 &&
                                 o.inner.compare(0, norm.size(), norm) == 0 &&
                                 o.inner[norm.size()] == L'\\');
                       }),
        g_app.edits.opened.end());

    if (wasAdded) continue;
    AddUnique(g_app.edits.deleted, norm);
  }
  return {};
}

std::wstring AppMakeEditable(int entryIndex, const std::wstring& entryPath) {
  if (!g_app.archiveMode || entryIndex < 0) return L"当前没有打开压缩包";
  std::wstring error;
  const std::wstring landed = StageEntry(entryIndex, entryPath, error);
  if (landed.empty()) return error;

  // 只是「解出来了」，不等于「改过了」：把解出时包内声明的大小与时间一起记下，
  // 导出前再逐个核实。这样用户点开看一眼不会让界面冒出「导出压缩包」。
  const std::wstring norm = App::NormalizeInner(entryPath);
  bool tracked = InAdded(norm);
  for (const auto& o : g_app.edits.opened) {
    if (o.inner == norm) tracked = true;
  }
  if (!tracked) {
    const auto& entries = g_app.archive.Entries();
    if (entryIndex < static_cast<int>(entries.size())) {
      const auto& e = entries[static_cast<size_t>(entryIndex)];
      g_app.edits.opened.push_back({norm, e.size, e.modifiedMillis, e.hasMtime});
    } else {
      g_app.edits.opened.push_back({norm, 0, 0, false});
    }
  }

  if (!AiUI::platform()->openWithSystem(landed)) return L"没有程序可以打开这个文件";
  return {};
}

void AppScanModifiedEntries() {
  // 用户可能在外部编辑器里改完存盘，程序无从得知，只能按大小与时间比对。
  // 解压时会回写原修改时间，所以时间不同就是用户在外部动过。
  //
  // 只检查 opened（点开过的）而不是整包：没点开过的文件根本不可能被改，
  // 而十万条的包逐个 stat 会让导出前那一下明显卡顿。
  for (const auto& o : g_app.edits.opened) {
    if (IsDeletedPath(o.inner) || InAdded(o.inner)) continue;

    const std::wstring disk = StagePathFor(o.inner);
    if (disk.empty() || !util::PathExists(disk) || util::IsDirectory(disk)) continue;

    const uint64_t size = AiUI::platform()->fileSize(disk);
    const int64_t mtime = AiUI::platform()->modifiedTime(disk);
    if (size != o.size || (o.hasMtime && mtime != o.modifiedMillis)) {
      AddUnique(g_app.edits.modified, o.inner);
    }
  }
}

// ── 编辑清单查询 ──
//
// 左侧树按包内条目建节点，但清单里的增删只存在于编辑会话中：
// 不查这三项，删掉的目录会一直挂在树上，新增的目录永远看不见。

bool AppIsPathDeleted(const std::wstring& normalized) { return IsDeletedPath(normalized); }

std::vector<std::wstring> AppAddedChildNames(const std::wstring& normalizedDir) {
  std::vector<std::wstring> out;
  const std::wstring prefix =
      normalizedDir.empty() ? std::wstring() : normalizedDir + L"\\";

  for (const auto& raw : g_app.edits.added) {
    const std::wstring norm = App::NormalizeInner(raw);
    if (norm.empty()) continue;
    if (!prefix.empty() && !AiUI::platform()->isPathPrefix(prefix, norm)) continue;

    // 相对当前这一层的第一段就是子项名。名字要用原始大小写，所以从 raw
    // 里按相同位置截取，而不是拿 norm 显示。
    std::wstring display = raw;
    std::replace(display.begin(), display.end(), L'/', L'\\');
    while (!display.empty() && display.front() == L'\\') display.erase(display.begin());
    if (!prefix.empty()) {
      if (display.size() <= prefix.size()) continue;
      display = display.substr(prefix.size());
    }
    const size_t slash = display.find(L'\\');
    if (slash != std::wstring::npos) display = display.substr(0, slash);
    if (display.empty()) continue;

    // 树只挂目录，和原有条目那条分支保持同一个口径：
    // 新增文件不是节点，混进来会变成「点不开的目录」。
    const std::wstring childInner = prefix.empty() ? display : prefix + display;
    const std::wstring childDisk = StagePathFor(childInner);
    if (childDisk.empty() || !util::IsDirectory(childDisk)) continue;

    bool exists = false;
    for (const auto& v : out) {
      if (AiUI::platform()->equalsPath(v, display)) {
        exists = true;
        break;
      }
    }
    if (!exists) out.push_back(display);
  }
  return out;
}

std::wstring AppExportArchive() {
  if (!g_app.archiveMode) return L"当前没有打开压缩包";
  if (AppInNestedArchive()) {
    return L"包内的压缩包是预览用的临时副本，改动无法保存。\n请先返回上一层再导出。";
  }
  AppScanModifiedEntries();
  if (!g_app.HasPendingEdits()) return {};

  const std::wstring original = g_app.archivePath;
  const std::wstring workRoot =
      util::JoinPath(AiUI::platform()->tempDir(),
                     L"aizip_export_" + std::to_wstring(AiUI::platform()->processId()) + L"_" +
                         std::to_wstring(AiUI::platform()->monotonicMillis()));
  const std::wstring unpackDir = util::JoinPath(workRoot, L"unpack");
  const std::wstring outDir = util::JoinPath(workRoot, L"out");
  const std::wstring outPath = util::JoinPath(outDir, util::BaseName(original));

  std::wstring password;
  std::wstring error;
  const auto deleted = g_app.edits.deleted;
  const auto added = g_app.edits.added;
  const auto modified = g_app.edits.modified;
  const std::wstring stage = g_app.stageDir;

  const bool ok = ui::ShowProgressDialog(g_app.mainWnd, L"正在导出压缩包", original,
                                         [&](const arc::ProgressFn& prog) {
    if (!util::EnsureDirectory(unpackDir) || !util::EnsureDirectory(outDir)) {
      error = L"无法创建临时目录";
      return false;
    }

    // 1) 解出原包中「没被删除」的条目。这是第一次全量解压——打开包时什么都没解，
    //    所以超大包的打开速度不受影响。用 indices 精确跳过删除项，
    //    省掉「解了再删」的无效工作。
    std::vector<uint32_t> keep;
    const auto& entries = g_app.archive.Entries();
    keep.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
      if (!IsDeletedPath(App::NormalizeInner(entries[i].path))) {
        keep.push_back(static_cast<uint32_t>(i));
      }
    }

    if (!keep.empty()) {
      arc::ExtractOptions opts;
      opts.targetDir = unpackDir;
      if (!g_app.archive.Extract(
              keep, opts, prog, nullptr,
              [&password](std::wstring& out) -> bool {
                bool accepted = false;
                ui::RunOnUiThread([&] {
                  std::wstring pwd;
                  accepted = ui::PromptText(g_app.mainWnd, L"输入密码",
                                            L"压缩包已加密，请输入密码：", pwd, true);
                  if (accepted) {
                    pwd.swap(password);
                    out = password;
                  }
                });
                return accepted;
              },
              error)) {
        if (error.empty()) error = L"无法解开原压缩包";
        return false;
      }
    }

    // 2) 把镜像里新增/改动的条目盖到解压结果上。
    //
    //    目录只需建出来（子项各自在清单里，会逐个处理）；文件直接覆盖同名项。
    //    不能对目录做整树复制——addtive 清单里已经有它的每一个子项，
    //    整树复制会把同一批文件搬两遍。
    if (!stage.empty() && util::PathExists(stage)) {
      for (const auto& inner : added) {
        const std::wstring src = util::JoinPath(stage, inner);
        if (!util::PathExists(src)) continue;
        const std::wstring dst = util::JoinPath(unpackDir, inner);
        if (util::IsDirectory(src)) {
          util::EnsureDirectory(dst);
          continue;
        }
        util::EnsureDirectory(util::DirName(dst));
        if (!util::CopyFileTo(src, dst)) {
          error = L"无法写入：" + inner;
          return false;
        }
      }
      for (const auto& inner : modified) {
        const std::wstring src = util::JoinPath(stage, inner);
        if (!util::PathExists(src) || util::IsDirectory(src)) continue;
        const std::wstring dst = util::JoinPath(unpackDir, inner);
        util::EnsureDirectory(util::DirName(dst));
        if (!util::CopyFileTo(src, dst)) {
          error = L"无法写入：" + inner;
          return false;
        }
      }
    }

    // 3) 压回同格式。输出先写临时目录，成功后才替换原文件。
    std::vector<std::wstring> contents;
    for (const auto& entry : AiUI::platform()->listDirectory(unpackDir, false)) {
      contents.push_back(entry.fullPath);
    }
    if (contents.empty()) {
      error = L"导出后压缩包会是空的，已取消";
      return false;
    }

    arc::CompressOptions copts;
    copts.archivePath = outPath;
    copts.password = password;
    if (!arc::Compress(contents, copts, prog, error)) {
      if (error.empty()) error = L"重新压缩失败";
      return false;
    }
    return true;
  });

  if (!ok) {
    util::RemoveDirectoryTree(workRoot);
    return error.empty() ? L"导出失败" : error;
  }

  // 替换原包：先关句柄，否则 Windows 上换不掉
  g_app.archive.Close();
  bool replaced = false;
  for (int attempt = 0; attempt < 10 && !replaced; ++attempt) {
    replaced = AiUI::platform()->moveFile(outPath, original);
    if (!replaced) AiUI::platform()->sleepMillis(50);
  }
  if (!replaced) replaced = util::CopyFileTo(outPath, original);
  util::RemoveDirectoryTree(workRoot);

  if (!replaced) {
    arc::OpenResult r;
    OpenArchiveWithPassword(g_app.archive, original, DialogOwner(), r);
    AppRefreshView();
    return L"无法写回压缩包，原文件可能被占用";
  }

  // 包变了，镜像与待办清单都已过期
  util::RemoveDirectoryTree(g_app.stageDir);
  g_app.stageDir.clear();
  g_app.edits = App::EditSession{};

  arc::OpenResult r;
  if (!OpenArchiveWithPassword(g_app.archive, original, DialogOwner(), r)) {
    g_app.archiveMode = false;
    g_app.archivePath.clear();
    g_app.innerPath.clear();
    AppRefreshView();
    return r.error.empty() ? L"写回后无法重新打开压缩包" : r.error;
  }
  AppRefreshView();
  return {};
}

void AppDiscardEdits() {
  util::RemoveDirectoryTree(g_app.stageDir);
  g_app.stageDir.clear();
  g_app.edits = App::EditSession{};
}

bool AppConfirmDiscardEdits(AiUI::NativeHandle owner) {
  if (!g_app.HasPendingEdits()) return true;
  AiUI::MessageBoxParams params;
  params.title = L"爱压";
  params.message =
      L"压缩包还有未导出的改动。\n\n直接离开会丢弃这些改动，原压缩包保持原样。\n"
      L"要继续吗？";
  params.icon = AiUI::MessageBoxIcon::kWarning;
  params.buttons = AiUI::MessageBoxButtons::kYesNo;
  params.defaultButton = AiUI::MessageBoxResult::kNo;
  return AiUI::platform()->showMessageBox(owner, params) == AiUI::MessageBoxResult::kYes;
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
    // 本层包根：在嵌套里就先退回外层那个包（它才是「上一层」），
    // 只有真外层才是磁盘目录
    if (AppLeaveNestedArchive()) return;
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
      AppClearNestedArchives();
      arc::OpenResult r;
      if (OpenArchiveWithPassword(g_app.archive, target, DialogOwner(), r)) {
        util::RemoveDirectoryTree(g_app.stageDir);
        g_app.stageDir.clear();
        g_app.archiveMode = true;
        g_app.homeMode = false;
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
      AppClearNestedArchives();
      arc::OpenResult r;
      if (OpenArchiveWithPassword(g_app.archive, target, DialogOwner(), r)) {
        util::RemoveDirectoryTree(g_app.stageDir);
        g_app.stageDir.clear();
        g_app.archiveMode = true;
        g_app.homeMode = false;
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
