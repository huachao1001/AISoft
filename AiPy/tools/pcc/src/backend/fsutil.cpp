// 文件与路径工具的实现（Windows）。
//
// ── 为什么全部走宽字符 ──
//
// 用户脚本与输出路径都可能是中文，而 ANSI API 按当前代码页解释字节，
// 在 GBK 控制台与 UTF-8 脚本同时存在时会失败或乱码。内部一律转成
// UTF-16 再调 Win32/CRT 的宽字符版本。
#include "pcc/backend/fsutil.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pcc {
namespace {

#ifdef _WIN32

std::wstring toWide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
  if (length <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), &wide[0], length);
  return wide;
}

std::string toUtf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
  if (length <= 0) return std::string();
  std::string text(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()), &text[0], length,
                        nullptr, nullptr);
  return text;
}

#endif

}  // namespace

bool fileExists(const std::string& path) {
  const std::wstring wide = toWide(path);
  const DWORD attributes = ::GetFileAttributesW(wide.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool directoryExists(const std::string& path) {
  const std::wstring wide = toWide(path);
  const DWORD attributes = ::GetFileAttributesW(wide.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool readFileBytes(const std::string& path, std::vector<std::uint8_t>* out) {
  const std::wstring wide = toWide(path);
  std::FILE* handle = _wfopen(wide.c_str(), L"rb");
  if (handle == nullptr) return false;
  std::fseek(handle, 0, SEEK_END);
  const long size = std::ftell(handle);
  if (size < 0) {
    std::fclose(handle);
    return false;
  }
  std::fseek(handle, 0, SEEK_SET);
  out->resize(static_cast<std::size_t>(size));
  const std::size_t got =
      size == 0 ? 0 : std::fread(out->data(), 1, out->size(), handle);
  std::fclose(handle);
  return got == out->size();
}

bool readFileText(const std::string& path, std::string* out) {
  std::vector<std::uint8_t> bytes;
  if (!readFileBytes(path, &bytes)) return false;
  out->assign(bytes.begin(), bytes.end());
  return true;
}

bool writeFileBytes(const std::string& path,
                    const std::vector<std::uint8_t>& data) {
  const std::wstring wide = toWide(path);
  std::FILE* handle = _wfopen(wide.c_str(), L"wb");
  if (handle == nullptr) return false;
  const std::size_t written =
      data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), handle);
  std::fclose(handle);
  return written == data.size();
}

bool writeFileText(const std::string& path, const std::string& text) {
  return writeFileBytes(
      path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::uint64_t fileSize(const std::string& path) {
  const std::wstring wide = toWide(path);
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!::GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) {
    return 0;
  }
  return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) |
         data.nFileSizeLow;
}

std::uint64_t lastWriteTime(const std::string& path) {
  const std::wstring wide = toWide(path);
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!::GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) {
    return 0;
  }
  return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
         data.ftLastWriteTime.dwLowDateTime;
}

std::vector<std::string> listDirectory(const std::string& path) {
  std::vector<std::string> names;
  const std::string pattern = joinPath(path, "*");
  const std::wstring wide = toWide(pattern);
  WIN32_FIND_DATAW data;
  HANDLE handle = ::FindFirstFileW(wide.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) return names;
  do {
    const std::wstring name = data.cFileName;
    if (name == L"." || name == L"..") continue;
    names.push_back(toUtf8(name));
  } while (::FindNextFileW(handle, &data));
  ::FindClose(handle);
  return names;
}

std::string joinPath(const std::string& base, const std::string& name) {
  if (base.empty()) return name;
  if (name.empty()) return base;
  const char last = base[base.size() - 1];
  if (last == '\\' || last == '/') return base + name;
  return base + pathSeparator() + name;
}

std::string dirName(const std::string& path) {
  const std::size_t at = path.find_last_of("\\/");
  if (at == std::string::npos) return std::string();
  if (at == 0) return path.substr(0, 1);
  return path.substr(0, at);
}

std::string baseName(const std::string& path) {
  const std::size_t at = path.find_last_of("\\/");
  if (at == std::string::npos) return path;
  return path.substr(at + 1);
}

std::string absolutePath(const std::string& path) {
  const std::wstring wide = toWide(path);
  const DWORD needed = ::GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
  if (needed == 0) return path;
  std::wstring full(needed, L'\0');
  const DWORD written = ::GetFullPathNameW(wide.c_str(), needed, &full[0], nullptr);
  if (written == 0) return path;
  full.resize(written);
  return toUtf8(full);
}

std::string normalizePath(const std::string& path) {
  std::string text;
  text.reserve(path.size());
  for (char ch : path) {
    text.push_back(ch == '/' ? '\\' : ch);
  }
  // 去掉末尾分隔符（`C:\` 这种根目录保留）。
  while (text.size() > 1 && text.back() == '\\' && text[text.size() - 2] != ':') {
    text.pop_back();
  }
  return text;
}

std::string extensionOf(const std::string& path) {
  const std::string name = baseName(path);
  const std::size_t at = name.find_last_of('.');
  if (at == std::string::npos || at == 0) return std::string();
  std::string extension = name.substr(at);
  for (char& ch : extension) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return extension;
}

std::string stripExtension(const std::string& path) {
  const std::size_t at = path.find_last_of('.');
  const std::size_t slash = path.find_last_of("\\/");
  if (at == std::string::npos) return path;
  if (slash != std::string::npos && at < slash) return path;
  return path.substr(0, at);
}

std::string pathSeparator() { return "\\"; }

std::vector<std::string> splitList(const std::string& text) {
  std::vector<std::string> parts;
  std::string current;
  for (char ch : text) {
    if (ch == ';') {
      if (!current.empty()) parts.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

std::string tempDirectory() {
  wchar_t buffer[MAX_PATH];
  const DWORD length = ::GetTempPathW(MAX_PATH, buffer);
  if (length == 0) return ".";
  std::wstring wide(buffer, length);
  while (!wide.empty() && (wide.back() == L'\\' || wide.back() == L'/')) {
    wide.pop_back();
  }
  return toUtf8(wide);
}

}  // namespace pcc
