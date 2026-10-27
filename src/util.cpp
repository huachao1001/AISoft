#include "util.h"

#include <shlwapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>

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

std::wstring NormalizePath(const std::wstring& path) {
  wchar_t buf[MAX_PATH * 2];
  if (::PathCanonicalizeW(buf, path.c_str())) return buf;
  return path;
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

std::wstring FormatCount(size_t n) {
  std::wstring s = std::to_wstring(n);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, L",");
  return s;
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

int ScaleDpi(int value, int dpi) { return ::MulDiv(value, dpi, 96); }

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
constexpr const wchar_t* kRegPath = L"Software\\MyZip";
}

Settings LoadSettings() {
  Settings s;
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return s;

  auto readStr = [&](const wchar_t* name, std::wstring& out) {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) ==
            ERROR_SUCCESS &&
        type == REG_SZ) {
      out.assign(buf, size / sizeof(wchar_t));
      while (!out.empty() && out.back() == L'\0') out.pop_back();
    }
  };
  auto readDword = [&](const wchar_t* name, int& out) {
    DWORD size = sizeof(DWORD);
    DWORD type = 0;
    DWORD v = 0;
    if (::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&v), &size) ==
            ERROR_SUCCESS &&
        type == REG_DWORD) {
      out = static_cast<int>(v);
    }
  };

  readStr(L"LastDir", s.lastDir);
  readStr(L"ExtractDir", s.extractDir);
  readStr(L"FormatName", s.compressFormat);
  readDword(L"Level", s.compressLevel);
  readDword(L"Solid", *reinterpret_cast<int*>(&s.solid));
  readDword(L"OpenAfterExtract", *reinterpret_cast<int*>(&s.openAfterExtract));
  readDword(L"ShowTree", *reinterpret_cast<int*>(&s.showTree));
  readDword(L"SortColumn", s.listSortColumn);
  readDword(L"SortAsc", *reinterpret_cast<int*>(&s.listSortAsc));
  readDword(L"WinX", s.winX);
  readDword(L"WinY", s.winY);
  readDword(L"WinW", s.winW);
  readDword(L"WinH", s.winH);
  readDword(L"WinMax", *reinterpret_cast<int*>(&s.winMax));
  ::RegCloseKey(key);
  return s;
}

void SaveSettings(const Settings& s) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
    return;
  }
  auto writeStr = [&](const wchar_t* name, const std::wstring& v) {
    ::RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                     static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
  };
  auto writeDword = [&](const wchar_t* name, int v) {
    DWORD d = static_cast<DWORD>(v);
    ::RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&d), sizeof(DWORD));
  };

  writeStr(L"LastDir", s.lastDir);
  writeStr(L"ExtractDir", s.extractDir);
  writeStr(L"FormatName", s.compressFormat);
  writeDword(L"Level", s.compressLevel);
  writeDword(L"Solid", s.solid ? 1 : 0);
  writeDword(L"OpenAfterExtract", s.openAfterExtract ? 1 : 0);
  writeDword(L"ShowTree", s.showTree ? 1 : 0);
  writeDword(L"SortColumn", s.listSortColumn);
  writeDword(L"SortAsc", s.listSortAsc ? 1 : 0);
  writeDword(L"WinX", s.winX);
  writeDword(L"WinY", s.winY);
  writeDword(L"WinW", s.winW);
  writeDword(L"WinH", s.winH);
  writeDword(L"WinMax", s.winMax ? 1 : 0);
  ::RegCloseKey(key);
}

}  // namespace util
