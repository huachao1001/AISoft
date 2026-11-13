// 系统右键菜单注册：动作清单在这里，落到哪个系统机制由平台层决定
#include "shell_menu.h"

#include "AiUI/shell.h"

#include <vector>

namespace shell {
namespace {

// 标识带上产品前缀，避免和别的压缩软件撞车；也决定菜单里的排序
constexpr const wchar_t* kIdCompress = L"AIZip.Compress";
constexpr const wchar_t* kIdCompressZip = L"AIZip.CompressZip";
constexpr const wchar_t* kIdOpen = L"AIZip.Browse";
constexpr const wchar_t* kIdExtract = L"AIZip.Extract";
constexpr const wchar_t* kIdExtractHere = L"AIZip.ExtractHere";

// 右键菜单里显示的文字
constexpr const wchar_t* kLabelCompress = L"压缩该文件...";
constexpr const wchar_t* kLabelCompressZip = L"压缩到 ZIP";
constexpr const wchar_t* kLabelOpen = L"打开压缩文件";
constexpr const wchar_t* kLabelExtract = L"解压该文件...";
constexpr const wchar_t* kLabelExtractHere = L"解压到当前目录";

// 命令行开关：系统通过它告诉程序该做什么
constexpr const wchar_t* kArgCompress = L"-compress";
constexpr const wchar_t* kArgCompressZip = L"-compress-zip";
constexpr const wchar_t* kArgExtract = L"-extract";
constexpr const wchar_t* kArgExtractHere = L"-extract-here";

// 支持的压缩包后缀，和 Archive::IsArchiveFile 保持一致
const std::vector<std::wstring>& ArchiveExts() {
  static const std::vector<std::wstring> kExts = {
      L".zip",  L".7z",   L".rar",  L".tar",  L".gz",  L".tgz",  L".bz2", L".tbz2",
      L".xz",   L".txz",  L".cab",  L".iso",  L".wim", L".jar",  L".apk", L".aar",
      L".epub", L".lzh",  L".arj",  L".z",    L".001"};
  return kExts;
}

// 完整动作清单。
//
// 压缩项挂在"所有文件"上且不排除任何后缀。原因是多选时 shell 的 AppliesTo
// 只按第一个选中项判定：一旦首个文件是 .zip，排除条件就会让整个压缩项消失，
// 用户没法把多个压缩包重新打包。单选的取舍是包里也会看到压缩项，
// 这比"多选时功能消失"更容易接受。
std::vector<AiUI::ShellVerb> Verbs() {
  std::vector<AiUI::ShellVerb> verbs;

  AiUI::ShellVerb compress;
  compress.id = kIdCompress;
  compress.label = kLabelCompress;
  compress.command = kArgCompress;
  // 不置顶：按 id 排序（Compress → CompressZip），两项自然挨在一起
  verbs.push_back(compress);

  AiUI::ShellVerb quickZip;
  quickZip.id = kIdCompressZip;
  quickZip.label = kLabelCompressZip;
  quickZip.command = kArgCompressZip;
  verbs.push_back(quickZip);

  // 压缩包后缀挂解压项。按类型挂动词，不碰默认打开方式，
  // 避免抢走用户的文件关联。

  // 「打开压缩文件」：不带动作开关，等于让程序按默认方式浏览包内内容，
  // 与「打开方式」选 AIZip 走同一条路径，因此会复用已开的主窗口。
  // id 取 Browse：菜单按子键名字母序排，B 排在 C/E 之前。
  AiUI::ShellVerb open;
  open.id = kIdOpen;
  open.label = kLabelOpen;
  open.targets = ArchiveExts();
  // 多选时"打开一个包"没有意义，只留给单选
  open.singleSelectionOnly = true;
  verbs.push_back(open);

  AiUI::ShellVerb extract;
  extract.id = kIdExtract;
  extract.label = kLabelExtract;
  extract.command = kArgExtract;
  extract.targets = ArchiveExts();
  extract.singleSelectionOnly = true;
  verbs.push_back(extract);

  // 「解压到当前目录」挂同样的后缀，同样只在单选时出现：
  // 多选多个包时"当前目录"指哪个包并不明确。
  AiUI::ShellVerb extractHere;
  extractHere.id = kIdExtractHere;
  extractHere.label = kLabelExtractHere;
  extractHere.command = kArgExtractHere;
  extractHere.targets = ArchiveExts();
  extractHere.singleSelectionOnly = true;
  verbs.push_back(extractHere);

  return verbs;
}

}  // namespace

std::wstring RegisterContextMenu() {
  // 旧版「打开压缩文件」用过 AIZip.Open，改名后必须清掉旧键，
  // 否则菜单里会同时出现两项。
  AiUI::ShellVerb legacy;
  legacy.id = L"AIZip.Open";
  legacy.label = L"打开压缩文件";
  legacy.targets = ArchiveExts();
  AiUI::unregisterShellVerbs({legacy});

  return AiUI::registerShellVerbs(Verbs());
}

std::wstring UnregisterContextMenu() {
  return AiUI::unregisterShellVerbs(Verbs());
}

bool IsContextMenuRegistered() {
  return AiUI::areShellVerbsRegistered(Verbs());
}

}  // namespace shell
