#include "util.h"

#include <shlobj.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace util {

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
  if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
  size_t i = 0;
  while (i < b.size() && (b[i] == L'\\' || b[i] == L'/')) ++i;
  out.append(b.substr(i));
  return out;
}

bool PathExists(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
  const DWORD a = ::GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirectory(const std::wstring& path) {
  if (path.empty()) return false;
  if (IsDirectory(path)) return true;
  const int r = ::SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
  return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

std::wstring AbsolutePath(const std::wstring& path) {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = ::GetFullPathNameW(path.c_str(), MAX_PATH * 2, buf, nullptr);
  if (n == 0 || n >= MAX_PATH * 2) return path;
  return std::wstring(buf, n);
}

namespace {
void CollectRecursive(const std::wstring& baseDir, const std::wstring& rel,
                      std::vector<SourceItem>& out) {
  const std::wstring pattern = JoinPath(baseDir, L"*");
  WIN32_FIND_DATAW fd;
  HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
    SourceItem item;
    item.fullPath = JoinPath(baseDir, fd.cFileName);
    item.relPath = rel.empty() ? fd.cFileName : JoinPath(rel, fd.cFileName);
    item.attrib = fd.dwFileAttributes;
    item.mtime = fd.ftLastWriteTime;
    item.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    item.size = item.isDir ? 0
                           : (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    out.push_back(item);
    if (item.isDir) CollectRecursive(item.fullPath, item.relPath, out);
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
}
}  // namespace

void CollectSources(const std::vector<std::wstring>& paths, std::vector<SourceItem>& out) {
  for (const auto& p : paths) {
    const DWORD attrib = ::GetFileAttributesW(p.c_str());
    if (attrib == INVALID_FILE_ATTRIBUTES) continue;

    SourceItem root;
    root.fullPath = p;
    root.relPath = BaseName(p);
    root.attrib = attrib;
    root.isDir = (attrib & FILE_ATTRIBUTE_DIRECTORY) != 0;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (::GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) {
      root.mtime = fad.ftLastWriteTime;
      root.size = root.isDir ? 0
                             : (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }
    out.push_back(root);

    if (root.isDir) CollectRecursive(p, root.relPath, out);
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

std::wstring FormatTime(const FILETIME& ft) {
  FILETIME local;
  if (!::FileTimeToLocalFileTime(&ft, &local)) return L"";
  SYSTEMTIME st;
  if (!::FileTimeToSystemTime(&local, &st)) return L"";
  wchar_t buf[64];
  swprintf(buf, 64, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
  return buf;
}

std::wstring FormatCrc(uint32_t crc, bool has) {
  if (!has) return L"";
  wchar_t buf[16];
  swprintf(buf, 16, L"%08X", crc);
  return buf;
}

int DpiForWindow(HWND hwnd) {
  using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
  static auto fn = reinterpret_cast<GetDpiForWindowFn>(
      ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (fn && hwnd) return static_cast<int>(fn(hwnd));
  HDC dc = ::GetDC(nullptr);
  const int dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
  ::ReleaseDC(nullptr, dc);
  return dpi > 0 ? dpi : 96;
}

void EnableDpiAwareness() {
  using SetCtxFn = BOOL(WINAPI*)(void*);
  static auto setCtx = reinterpret_cast<SetCtxFn>(
      ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
  if (setCtx) {
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (setCtx(reinterpret_cast<void*>(-4))) return;
  }
  ::SetProcessDPIAware();
}

namespace {

// 配置放在用户漫游配置目录：%APPDATA%\AIZip\settings.ini。
// 不写注册表，便于备份/迁移，也避免污染系统库。
std::wstring ConfigDir() {
  PWSTR appData = nullptr;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) || !appData) {
    if (appData) ::CoTaskMemFree(appData);
    return L"";
  }
  std::wstring dir = JoinPath(appData, L"AIZip");
  ::CoTaskMemFree(appData);
  return dir;
}

std::wstring ConfigFile() {
  const std::wstring dir = ConfigDir();
  return dir.empty() ? L"" : JoinPath(dir, L"settings.ini");
}

// 自己解析而不是用 GetPrivateProfileString：后者按 ANSI 处理，
// 用户用记事本另存为 UTF-8（带 BOM）后节名就变成 "\xEF\xBB\xBF[settings]"，
// 整份配置立刻全部失效。这里固定按 UTF-16LE 读写，BOM 可有可无。
std::wstring TrimRight(std::wstring s) {
  while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' ||
                        s.back() == L'\t')) {
    s.pop_back();
  }
  return s;
}

// 读成 key → value；节名与大小写都不敏感地忽略，只认顶层键值
bool ParseIni(const std::wstring& path, std::map<std::wstring, std::wstring>& out) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;

  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 20)) {
    ::CloseHandle(h);
    return false;
  }
  std::vector<char> raw(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ::ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr);
  ::CloseHandle(h);
  if (!ok || read == 0) return false;

  // 统一转成宽字符：带 BOM 的 UTF-16 直接按 UTF-16 解，否则按 UTF-8
  std::wstring text;
  const auto* p = reinterpret_cast<const unsigned char*>(raw.data());
  const size_t n = read;
  if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
    text.assign(reinterpret_cast<const wchar_t*>(p + 2), (n - 2) / sizeof(wchar_t));
  } else if (n >= 2 && p[1] == 0 && p[0] != 0) {
    // 无 BOM 的 UTF-16LE：偶数位多为 0，按 UTF-16 解
    text.assign(reinterpret_cast<const wchar_t*>(p), n / sizeof(wchar_t));
  } else {
    const size_t skip = (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) ? 3 : 0;
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p + skip),
                                           static_cast<int>(n - skip), nullptr, 0);
    if (need > 0) {
      text.resize(static_cast<size_t>(need));
      ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p + skip),
                            static_cast<int>(n - skip), text.data(), need);
    }
  }
  if (text.empty()) return false;

  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t end = text.find(L'\n', pos);
    std::wstring line =
        TrimRight(text.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos));
    pos = (end == std::wstring::npos) ? text.size() + 1 : end + 1;

    if (line.empty() || line[0] == L'[' || line[0] == L';' || line[0] == L'#') continue;
    const size_t eq = line.find(L'=');
    if (eq == std::wstring::npos) continue;
    out[TrimRight(line.substr(0, eq))] = line.substr(eq + 1);
  }
  return true;
}

std::wstring StrOf(const std::map<std::wstring, std::wstring>& m, const wchar_t* key,
                   const wchar_t* def = L"") {
  const auto it = m.find(key);
  return it == m.end() ? def : it->second;
}

int IntOf(const std::map<std::wstring, std::wstring>& m, const wchar_t* key, int def) {
  const auto it = m.find(key);
  if (it == m.end() || it->second.empty()) return def;
  try {
    return std::stoi(it->second);
  } catch (...) {
    return def;
  }
}

void AppendUtf16(std::vector<char>& buf, const std::wstring& s) {
  const auto* p = reinterpret_cast<const char*>(s.data());
  buf.insert(buf.end(), p, p + s.size() * sizeof(wchar_t));
}

}  // namespace

Settings LoadSettings() {
  Settings s;
  const std::wstring file = ConfigFile();
  if (file.empty() || !PathExists(file)) return s;

  std::map<std::wstring, std::wstring> m;
  if (!ParseIni(file, m)) return s;

  s.lastDir = StrOf(m, L"LastDir");
  s.extractDir = StrOf(m, L"ExtractDir");
  const std::wstring fmt = StrOf(m, L"FormatName");
  if (!fmt.empty()) s.compressFormat = fmt;
  s.compressLevel = IntOf(m, L"Level", s.compressLevel);
  s.solid = IntOf(m, L"Solid", s.solid ? 1 : 0) != 0;
  s.openAfterExtract = IntOf(m, L"OpenAfterExtract", s.openAfterExtract ? 1 : 0) != 0;
  s.showTree = IntOf(m, L"ShowTree", s.showTree ? 1 : 0) != 0;
  s.listSortColumn = IntOf(m, L"SortColumn", s.listSortColumn);
  s.listSortAsc = IntOf(m, L"SortAsc", s.listSortAsc ? 1 : 0) != 0;
  s.winW = IntOf(m, L"WinW", s.winW);
  s.winH = IntOf(m, L"WinH", s.winH);
  s.winMax = IntOf(m, L"WinMax", s.winMax ? 1 : 0) != 0;
  return s;
}

void SaveSettings(const Settings& s) {
  const std::wstring dir = ConfigDir();
  if (dir.empty() || !EnsureDirectory(dir)) return;
  const std::wstring file = JoinPath(dir, L"settings.ini");

  auto key = [](const wchar_t* k, const std::wstring& v) {
    std::wstring line = k;
    line += L'=';
    line += v;
    line += L"\r\n";
    return line;
  };
  std::wstring text = L"[settings]\r\n";
  text += key(L"LastDir", s.lastDir);
  text += key(L"ExtractDir", s.extractDir);
  text += key(L"FormatName", s.compressFormat);
  text += key(L"Level", std::to_wstring(s.compressLevel));
  text += key(L"Solid", std::to_wstring(s.solid ? 1 : 0));
  text += key(L"OpenAfterExtract", std::to_wstring(s.openAfterExtract ? 1 : 0));
  text += key(L"ShowTree", std::to_wstring(s.showTree ? 1 : 0));
  text += key(L"SortColumn", std::to_wstring(s.listSortColumn));
  text += key(L"SortAsc", std::to_wstring(s.listSortAsc ? 1 : 0));
  text += key(L"WinW", std::to_wstring(s.winW));
  text += key(L"WinH", std::to_wstring(s.winH));
  text += key(L"WinMax", std::to_wstring(s.winMax ? 1 : 0));

  // 固定写 UTF-16LE + BOM：路径里的中文不会因代码页变化而损坏
  std::vector<char> buf;
  const wchar_t bom = 0xFEFF;
  buf.insert(buf.end(), reinterpret_cast<const char*>(&bom),
             reinterpret_cast<const char*>(&bom) + sizeof(bom));
  AppendUtf16(buf, text);

  HANDLE h = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  ::WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr);
  ::CloseHandle(h);
}

}  // namespace util
