// 右键菜单内容：选中了什么 → 显示哪些项
//
// 这段代码被编进资源管理器进程内的组件（见 aiui_add_shell_menu_dll），所以
// 它必须完全自包含：不链接压缩内核、不依赖 AiUI 平台后端、只做纯字符串计算。
// 路径是否为目录由后端在 ShellSelectionItem 里填好。
#include "AiUI/shell_menu.h"

#include <string>
#include <vector>

namespace {

// 与 arc::Archive::IsArchiveFile 保持同一张表；改一处必须同步另一处。
// 组件里不能链接压缩内核，所以只能各自持有一份。
constexpr const wchar_t* kArchiveExts[] = {
    L".zip",  L".7z",   L".rar", L".tar", L".gz",  L".tgz", L".bz2", L".tbz2",
    L".xz",   L".txz",  L".cab", L".iso", L".wim", L".jar", L".apk", L".aar",
    L".epub", L".lzh",  L".arj", L".z",   L".001",
};

wchar_t lower(wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c; }

bool equalsNoCase(const std::wstring& a, const std::wstring& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

// 擅长处理两种分隔符：磁盘路径用 '\'，压缩包内成员用 '/'
size_t lastSeparator(const std::wstring& path) { return path.find_last_of(L"\\/"); }

// 去目录，并去掉结尾分隔符
std::wstring baseName(const std::wstring& path) {
  const size_t sep = lastSeparator(path);
  std::wstring name = (sep == std::wstring::npos) ? path : path.substr(sep + 1);
  while (!name.empty() && (name.back() == L'\\' || name.back() == L'/')) name.pop_back();
  return name;
}

// 去目录去扩展名
std::wstring fileStem(const std::wstring& path) {
  const std::wstring name = baseName(path);
  const size_t dot = name.find_last_of(L'.');
  return (dot == std::wstring::npos) ? name : name.substr(0, dot);
}

std::wstring dirName(const std::wstring& path) {
  const size_t sep = lastSeparator(path);
  if (sep == std::wstring::npos) return {};
  return path.substr(0, sep);
}

// 含点的小写扩展名
std::wstring fileExtension(const std::wstring& path) {
  const std::wstring name = baseName(path);
  const size_t dot = name.find_last_of(L'.');
  if (dot == std::wstring::npos || dot == 0) return {};
  std::wstring ext = name.substr(dot);
  for (auto& c : ext) c = lower(c);
  return ext;
}

bool isArchiveName(const std::wstring& path) {
  const std::wstring ext = fileExtension(path);
  if (ext.empty()) return false;
  for (const auto* candidate : kArchiveExts) {
    if (equalsNoCase(ext, candidate)) return true;
  }
  return false;
}

// 压缩输出名：单选取主名，多选取父目录名。
// 多选用第一个文件的名字会让用户以为只压了那一个，看不出整批都进去了。
std::wstring compressTargetName(const std::vector<AiUI::ShellSelectionItem>& selection) {
  if (selection.empty()) return L"archive";

  if (selection.size() > 1) {
    const std::wstring name = baseName(dirName(selection.front().path));
    return name.empty() ? L"archive" : name;
  }

  const auto& first = selection.front();
  const std::wstring name = first.isDirectory ? baseName(first.path) : fileStem(first.path);
  return name.empty() ? L"archive" : name;
}

}  // namespace

AiUI::ShellMenuResult aiuiBuildShellMenu(const std::vector<AiUI::ShellSelectionItem>& selection) {
  AiUI::ShellMenuResult result;
  if (selection.empty()) return result;

  bool anyArchive = false;
  bool allArchives = true;
  for (const auto& item : selection) {
    // 目录永远不算压缩包：打包一个目录才是常见诉求
    const bool isArchive = !item.isDirectory && isArchiveName(item.path);
    if (isArchive) anyArchive = true;
    else allArchives = false;
  }

  // 选中了压缩包就只给解压入口：再压一层不是用户想要的
  if (!anyArchive) {
    result.items.push_back({L"压缩到 " + compressTargetName(selection) + L".zip", L"-compress-zip",
                            false});
    result.items.push_back({L"添加到压缩文件...", L"-compress", true});
    return result;
  }

  // 解压只对单个压缩包有明确语义：多选时"解压到哪里"指向不明
  if (allArchives && selection.size() == 1) {
    result.items.push_back({L"打开压缩文件", L"", false});
    result.items.push_back({L"解压到...", L"-extract", false});
    result.items.push_back({L"解压到当前目录", L"-extract-here", true});
  }
  return result;
}
