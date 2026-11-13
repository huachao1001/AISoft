#include "util.h"

#include "AiUI/platform.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace util {
namespace {

AiUI::Platform* platform() { return AiUI::platform(); }

}  // namespace

std::wstring FileExtension(const std::wstring& path) {
  const size_t dot = path.find_last_of(L'.');
  const size_t sep = path.find_last_of(L"\\/");
  if (dot == std::wstring::npos) return L"";
  if (sep != std::wstring::npos && dot < sep) return L"";
  std::wstring ext = path.substr(dot);
  for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
  return ext;
}

std::wstring FileStem(const std::wstring& path) {
  const std::wstring base = BaseName(path);
  const size_t dot = base.find_last_of(L'.');
  return (dot == std::wstring::npos) ? base : base.substr(0, dot);
}

std::wstring ArchiveStem(const std::wstring& path) {
  // 组合后缀要连续剥两段，"a.tar.gz" 才算 a
  static const wchar_t* kDoubleExts[] = {L".tar.gz", L".tar.bz2", L".tar.xz",
                                         L".tar.zst", L".tar.z"};
  const std::wstring name = BaseName(path);
  std::wstring lowerName = name;
  for (auto& c : lowerName) {
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  }

  for (const wchar_t* ext : kDoubleExts) {
    const std::wstring suffix = ext;
    if (lowerName.size() > suffix.size() &&
        lowerName.compare(lowerName.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return name.substr(0, name.size() - suffix.size());
    }
  }

  const std::wstring stem = FileStem(path);
  return stem;
}

std::wstring BaseName(const std::wstring& path) {
  // 兼容压缩包内的 '/' 分隔
  const size_t sep = path.find_last_of(L"\\/");
  std::wstring name = (sep == std::wstring::npos) ? path : path.substr(sep + 1);
  while (!name.empty() && (name.back() == L'\\' || name.back() == L'/')) name.pop_back();
  return name;
}

std::wstring DirName(const std::wstring& path) {
  const size_t sep = path.find_last_of(L"\\/");
  if (sep == std::wstring::npos) return L"";
  if (sep == 2 && path[1] == L':') return path.substr(0, 3);
  return path.substr(0, sep);
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  std::wstring out = a;
  const wchar_t sep = platform()->pathSeparator();
  if (out.back() != sep && out.back() != L'/' && out.back() != L'\\') out.push_back(sep);
  size_t i = 0;
  while (i < b.size() && (b[i] == L'\\' || b[i] == L'/')) ++i;
  out.append(b.substr(i));
  return out;
}

bool PathExists(const std::wstring& path) { return platform()->pathExists(path); }

bool IsDirectory(const std::wstring& path) { return platform()->isDirectory(path); }

bool EnsureDirectory(const std::wstring& path) {
  if (path.empty()) return false;
  if (IsDirectory(path)) return true;
  // 逐级创建：平台接口只保证建一层
  std::wstring acc;
  for (size_t i = 0; i < path.size(); ++i) {
    acc.push_back(path[i]);
    const bool atSeparator = (path[i] == L'\\' || path[i] == L'/');
    const bool atDriveColon = (i == 1 && path[1] == L':');
    if (atSeparator || atDriveColon || i + 1 == path.size()) {
      if (!acc.empty() && !platform()->isDirectory(acc)) platform()->createDirectory(acc);
    }
  }
  return IsDirectory(path);
}

std::wstring AbsolutePath(const std::wstring& path) {
  // 已经是绝对路径（盘符或根分隔符）时原样返回
  if (path.size() >= 2 && path[1] == L':') return path;
  if (!path.empty() && (path.front() == L'\\' || path.front() == L'/')) return path;
  const std::wstring base = platform()->specialFolder(AiUI::SpecialFolder::kHome);
  return JoinPath(base, path);
}

namespace {

void collectRecursive(const std::wstring& baseDir, const std::wstring& rel,
                      std::vector<SourceItem>& out) {
  for (const auto& entry : platform()->listDirectory(baseDir, false)) {
    SourceItem item;
    item.fullPath = entry.fullPath;
    item.relPath = rel.empty() ? entry.name : JoinPath(rel, entry.name);
    item.modifiedMillis = entry.modifiedMillis;
    item.attributes = entry.attributes;
    item.isDir = entry.isDir;
    item.size = entry.isDir ? 0 : entry.size;
    out.push_back(item);
    if (item.isDir) collectRecursive(item.fullPath, item.relPath, out);
  }
}

}  // namespace

void CollectSources(const std::vector<std::wstring>& paths, std::vector<SourceItem>& out) {
  for (const auto& p : paths) {
    if (!platform()->pathExists(p)) continue;

    SourceItem root;
    root.fullPath = p;
    root.relPath = BaseName(p);
    root.isDir = platform()->isDirectory(p);
    root.size = root.isDir ? 0 : platform()->fileSize(p);
    root.modifiedMillis = platform()->modifiedTime(p);
    out.push_back(root);

    if (root.isDir) collectRecursive(p, root.relPath, out);
  }
}

std::wstring MakeUniquePath(const std::wstring& path) {
  if (!PathExists(path)) return path;
  const std::wstring dir = DirName(path);
  const std::wstring base = BaseName(path);
  const size_t dot = base.find_last_of(L'.');
  const std::wstring stem = (dot == std::wstring::npos) ? base : base.substr(0, dot);
  const std::wstring ext = (dot == std::wstring::npos) ? L"" : base.substr(dot);
  for (int i = 2; i < 10000; ++i) {
    const std::wstring candidate =
        JoinPath(dir, stem + L" (" + std::to_wstring(i) + L")" + ext);
    if (!PathExists(candidate)) return candidate;
  }
  return path;
}

std::wstring FormatSize(uint64_t bytes) {
  wchar_t buf[64];
  if (bytes < 1024) {
    swprintf(buf, 64, L"%llu B", static_cast<unsigned long long>(bytes));
  } else if (bytes < 1024ull * 1024) {
    swprintf(buf, 64, L"%.1f KB", bytes / 1024.0);
  } else if (bytes < 1024ull * 1024 * 1024) {
    swprintf(buf, 64, L"%.1f MB", bytes / (1024.0 * 1024));
  } else {
    swprintf(buf, 64, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
  }
  return buf;
}

std::wstring FormatTime(int64_t millis) {
  if (millis <= 0) return L"";
  // 由 Unix 纪元毫秒换算出日历时间；用标准库而非平台 API，跨平台一致
  const std::time_t seconds = static_cast<std::time_t>(millis / 1000);
  std::tm local{};
  if (localtime_s(&local, &seconds) != 0) return L"";
  wchar_t buf[64];
  swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", local.tm_year + 1900, local.tm_mon + 1,
           local.tm_mday, local.tm_hour, local.tm_min);
  return buf;
}

std::wstring FormatCrc(uint32_t crc, bool has) {
  if (!has) return L"";
  wchar_t buf[16];
  swprintf(buf, 16, L"%08X", crc);
  return buf;
}

std::string toUtf8(const std::wstring& text) {
  return AiUI::platform()->toUtf8(text);
}

std::wstring fromUtf8(const std::string& text) {
  return AiUI::platform()->fromUtf8(text);
}

namespace {

// 配置放在用户漫游配置目录：%APPDATA%\AIZip\settings.ini。
// 不写注册表，便于备份/迁移，也避免污染系统库。
std::wstring ConfigPath() {
  const std::wstring dir = platform()->appDataDir(L"AIZip");
  if (dir.empty()) return {};
  return JoinPath(dir, L"settings.ini");
}

}  // namespace

Settings LoadSettings() {
  Settings s;
  const std::wstring path = ConfigPath();
  if (path.empty()) return s;

  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return s;

  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    std::wstring w;
    // 按 UTF-8 读入；文件是纯 ASCII 键值，无需完整解码
    for (const char* p = line; *p; ++p) w.push_back(static_cast<wchar_t>(*p));
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
    const size_t eq = w.find(L'=');
    if (eq == std::wstring::npos) continue;
    const std::wstring key = w.substr(0, eq);
    const std::wstring value = w.substr(eq + 1);

    if (key == L"lastDir") s.lastDir = value;
    else if (key == L"extractDir") s.extractDir = value;
    else if (key == L"solid") s.solid = _wtoi(value.c_str()) != 0;
    else if (key == L"listSortColumn") s.listSortColumn = _wtoi(value.c_str());
    else if (key == L"listSortAsc") s.listSortAsc = _wtoi(value.c_str()) != 0;
    else if (key == L"winW") s.winW = _wtoi(value.c_str());
    else if (key == L"winH") s.winH = _wtoi(value.c_str());
    else if (key == L"winMax") s.winMax = _wtoi(value.c_str()) != 0;
    else if (key == L"theme") s.theme = value;
    else if (key.rfind(L"listColWidth", 0) == 0) {
      const int index = _wtoi(key.c_str() + 12);
      if (index >= 0 && index < 4) s.listColWidth[index] = _wtoi(value.c_str());
    }
  }
  fclose(f);
  return s;
}

void SaveSettings(const Settings& s) {
  const std::wstring path = ConfigPath();
  if (path.empty()) return;

  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;

  auto write = [f](const char* key, const std::wstring& value) {
    fprintf(f, "%s=", key);
    for (wchar_t c : value) fputc(static_cast<char>(c), f);
    fputc('\n', f);
  };
  auto writeInt = [f](const char* key, int value) { fprintf(f, "%s=%d\n", key, value); };

  write("lastDir", s.lastDir);
  write("extractDir", s.extractDir);
  // 格式与档位不持久化：压缩对话框每次打开都从 ZIP + 标准起步
  writeInt("solid", s.solid ? 1 : 0);
  writeInt("listSortColumn", s.listSortColumn);
  writeInt("listSortAsc", s.listSortAsc ? 1 : 0);
  writeInt("winW", s.winW);
  writeInt("winH", s.winH);
  writeInt("winMax", s.winMax ? 1 : 0);
  write("theme", s.theme);
  for (int i = 0; i < 4; ++i) {
    const std::string key = "listColWidth" + std::to_string(i);
    writeInt(key.c_str(), s.listColWidth[i]);
  }
  fclose(f);
}

}  // namespace util
