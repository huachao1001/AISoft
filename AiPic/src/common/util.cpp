#include "util.h"

#include "AiUI/platform.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace util {
namespace {

AiUI::Platform* platform() { return AiUI::platform(); }

// 配置放在用户漫游配置目录：%APPDATA%\AiPic\settings.ini。
// 不写注册表，便于备份/迁移，也避免污染系统库。
std::wstring ConfigPath() {
  const std::wstring dir = platform()->appDataDir(L"AiPic");
  if (dir.empty()) return {};
  return JoinPath(dir, L"settings.ini");
}

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

std::wstring BaseName(const std::wstring& path) {
  const size_t sep = path.find_last_of(L"\\/");
  return (sep == std::wstring::npos) ? path : path.substr(sep + 1);
}

std::wstring FileStem(const std::wstring& path) {
  const std::wstring base = BaseName(path);
  const size_t dot = base.find_last_of(L'.');
  return (dot == std::wstring::npos) ? base : base.substr(0, dot);
}

std::wstring DirName(const std::wstring& path) {
  const size_t sep = path.find_last_of(L"\\/");
  if (sep == std::wstring::npos) return L"";
  // 盘符根 "C:\" 不能退成 "C:"
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

std::wstring AbsolutePath(const std::wstring& path) { return platform()->absolutePath(path); }

bool PathExists(const std::wstring& path) { return platform()->pathExists(path); }

bool IsImageFile(const std::wstring& path) {
  static const wchar_t* kExts[] = {L".png", L".jpg",  L".jpeg", L".jpe",  L".bmp",
                                   L".gif", L".tif", L".tiff", L".ico", L".webp"};
  const std::wstring ext = FileExtension(path);
  if (ext.empty()) return false;
  for (const wchar_t* candidate : kExts) {
    if (ext == candidate) return true;
  }
  return false;
}

std::wstring FormatLabel(const std::wstring& path) {
  const std::wstring ext = FileExtension(path);
  if (ext == L".png") return L"PNG";
  if (ext == L".jpg" || ext == L".jpeg" || ext == L".jpe") return L"JPEG";
  if (ext == L".bmp") return L"BMP";
  if (ext == L".gif") return L"GIF";
  if (ext == L".tif" || ext == L".tiff") return L"TIFF";
  if (ext == L".ico") return L"ICO";
  if (ext == L".webp") return L"WEBP";
  if (ext.empty()) return L"";
  // 没有专门文案时退回去掉点的大写后缀，用户至少能认出自己存的是什么
  std::wstring label = ext.substr(1);
  for (auto& c : label) c = static_cast<wchar_t>(towupper(c));
  return label;
}

std::wstring FormatZoom(float zoom) {
  // 低于 100% 时保留一位小数：50% 与 52% 对用户是两回事
  const int percent = static_cast<int>(zoom * 100.0f + 0.5f);
  if (percent < 100) {
    const int tenth = static_cast<int>(zoom * 1000.0f + 0.5f) % 10;
    return std::to_wstring(percent) + L"." + std::to_wstring(tenth) + L"%";
  }
  return std::to_wstring(percent) + L"%";
}

Settings LoadSettings() {
  Settings s;
  const std::wstring path = ConfigPath();
  if (path.empty()) return s;

  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return s;

  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    std::string raw(line);
    while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) raw.pop_back();
    const size_t eq = raw.find('=');
    if (eq == std::string::npos) continue;
    // 键是纯 ASCII，直接比；值可能是 UTF-8 编码的中文路径，原样留着
    const std::string key = raw.substr(0, eq);
    const std::string value = raw.substr(eq + 1);

    if (key == "winW") s.winW = std::atoi(value.c_str());
    else if (key == "winH") s.winH = std::atoi(value.c_str());
    else if (key == "winMax") s.winMax = std::atoi(value.c_str()) != 0;
    else if (key == "theme") s.theme = platform()->fromUtf8(value);
    else if (key == "lastDir") s.lastDir = platform()->fromUtf8(value);
  }
  fclose(f);
  return s;
}

void SaveSettings(const Settings& s) {
  const std::wstring path = ConfigPath();
  if (path.empty()) return;

  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;

  auto writeValue = [f](const char* key, const std::string& value) {
    fprintf(f, "%s=%s\n", key, value.c_str());
  };
  auto writeInt = [f](const char* key, int value) { fprintf(f, "%s=%d\n", key, value); };

  writeInt("winW", s.winW);
  writeInt("winH", s.winH);
  writeInt("winMax", s.winMax ? 1 : 0);
  writeValue("theme", platform()->toUtf8(s.theme));
  // 目录可能含中文：按 UTF-8 落盘，读回来才不会变成问号
  writeValue("lastDir", platform()->toUtf8(s.lastDir));
  fclose(f);
}

}  // namespace util
