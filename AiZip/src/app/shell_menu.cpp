// 系统右键菜单注册：动作清单在这里，落到哪个系统机制由平台层决定
#include "shell_menu.h"

#include "AiUI/platform.h"

#include <vector>

namespace shell {
namespace {

// 动词名带上产品前缀，避免和别的压缩软件撞车
constexpr const wchar_t* kVerbCompress = L"AIZip.Compress";
constexpr const wchar_t* kVerbCompressZip = L"AIZip.CompressZip";
constexpr const wchar_t* kVerbExtract = L"AIZip.Extract";

// 右键菜单里显示的文字
constexpr const wchar_t* kLabelCompress = L"压缩该文件...";
constexpr const wchar_t* kLabelCompressZip = L"压缩到 ZIP";
constexpr const wchar_t* kLabelExtract = L"解压该文件...";

// 命令行开关：系统通过它告诉程序该做什么
constexpr const wchar_t* kArgCompress = L"-compress";
constexpr const wchar_t* kArgCompressZip = L"-compress-zip";
constexpr const wchar_t* kArgExtract = L"-extract";

// 支持的压缩包后缀，和 Archive::IsArchiveFile 保持一致
const std::vector<std::wstring>& ArchiveExts() {
  static const std::vector<std::wstring> kExts = {
      L".zip",  L".7z",  L".rar",  L".tar",  L".gz",   L".tgz", L".bz2",
      L".tbz2", L".xz",  L".txz",  L".cab",  L".iso",  L".wim", L".jar",
      L".apk",  L".epub", L".lzh", L".arj",  L".z",    L".001"};
  return kExts;
}

// 完整动作清单：所有文件与目录可压缩，压缩包后缀可解压
std::vector<AiUI::ContextMenuEntry> Entries() {
  std::vector<AiUI::ContextMenuEntry> entries;

  AiUI::ContextMenuEntry compress;
  compress.verb = kVerbCompress;
  compress.label = kLabelCompress;
  compress.command = kArgCompress;
  entries.push_back(compress);

  AiUI::ContextMenuEntry quickZip;
  quickZip.verb = kVerbCompressZip;
  quickZip.label = kLabelCompressZip;
  quickZip.command = kArgCompressZip;
  // 置顶：右键就能看到，不用展开级联菜单
  quickZip.topLevel = true;
  entries.push_back(quickZip);

  // 压缩包后缀挂解压项。按类型挂动词，不碰默认打开方式，
  // 避免抢走用户的文件关联。
  for (const auto& ext : ArchiveExts()) {
    AiUI::ContextMenuEntry extract;
    extract.verb = kVerbExtract;
    extract.label = kLabelExtract;
    extract.command = kArgExtract;
    extract.extensions = {ext};
    entries.push_back(extract);
  }
  return entries;
}

}  // namespace

std::wstring RegisterContextMenu() {
  return AiUI::platform()->registerContextMenu(Entries());
}

std::wstring UnregisterContextMenu() {
  return AiUI::platform()->unregisterContextMenu(Entries());
}

bool IsContextMenuRegistered() {
  return AiUI::platform()->isContextMenuRegistered(Entries());
}

}  // namespace shell
