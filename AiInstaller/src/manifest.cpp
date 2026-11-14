// 清单解析与序列化
//
// 只认「section + key=value」这一种语法：值里可以有 '='，但键与段名必须是
// ASCII。值用 UTF-8，这样中文标题不必依赖系统代码页。
//
// 本文件刻意不依赖 AiUI：打包器要在构建期跑，没必要为一个文本解析器把
// 整套图形库链接进去。UTF-8 与宽字符的互转就两段小循环，写在这里即可。
#include "AiInstaller/manifest.h"

#include <algorithm>
#include <cwchar>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace AiInstaller {
namespace {

using KeyValues = std::map<std::string, std::wstring>;

std::wstring fromUtf8(const std::string& text) {
  std::wstring out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      out.push_back(static_cast<wchar_t>(c));
      i += 1;
    } else if ((c >> 5) == 0x6 && i + 1 < text.size()) {
      out.push_back(static_cast<wchar_t>(((c & 0x1F) << 6) |
                                         (static_cast<unsigned char>(text[i + 1]) & 0x3F)));
      i += 2;
    } else if ((c >> 4) == 0xE && i + 2 < text.size()) {
      out.push_back(static_cast<wchar_t>(((c & 0x0F) << 12) |
                                         ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
                                         (static_cast<unsigned char>(text[i + 2]) & 0x3F)));
      i += 3;
    } else {
      // 非法序列跳过一字节，避免死循环
      i += 1;
    }
  }
  return out;
}

std::string toUtf8(const std::wstring& text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t c : text) {
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (c >> 12)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

std::wstring trim(const std::wstring& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) ++begin;
  while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t' ||
                         text[end - 1] == L'\r'))
    --end;
  return text.substr(begin, end - begin);
}

std::string lowerAscii(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

// 段名与键只可能是 ASCII，逐字符收窄即可
std::string narrowAscii(const std::wstring& text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t c : text) {
    if (c < 0x80) out.push_back(static_cast<char>(c));
  }
  return out;
}

bool parseIni(const std::wstring& text, KeyValues& out, std::wstring& error) {
  std::string section;
  size_t pos = 0;
  int lineNo = 0;

  while (pos <= text.size()) {
    const size_t eol = text.find(L'\n', pos);
    const size_t stop = eol == std::wstring::npos ? text.size() : eol;
    std::wstring line = trim(text.substr(pos, stop - pos));
    pos = eol == std::wstring::npos ? text.size() + 1 : eol + 1;
    ++lineNo;

    if (line.empty() || line[0] == L';' || line[0] == L'#') continue;

    if (line.front() == L'[') {
      const size_t close = line.find(L']');
      if (close == std::wstring::npos) {
        error = L"第 " + std::to_wstring(lineNo) + L" 行：段名缺少 ]";
        return false;
      }
      section = lowerAscii(narrowAscii(trim(line.substr(1, close - 1))));
      continue;
    }

    const size_t eq = line.find(L'=');
    if (eq == std::wstring::npos) {
      error = L"第 " + std::to_wstring(lineNo) + L" 行：不是 key=value";
      return false;
    }

    const std::string key = lowerAscii(narrowAscii(trim(line.substr(0, eq))));
    if (key.empty()) {
      error = L"第 " + std::to_wstring(lineNo) + L" 行：键为空";
      return false;
    }
    if (section.empty()) {
      error = L"第 " + std::to_wstring(lineNo) + L" 行：键不在任何段内";
      return false;
    }
    out[section + "." + key] = trim(line.substr(eq + 1));
  }
  return true;
}

std::wstring getText(const KeyValues& values, const char* key, const std::wstring& fallback) {
  const auto it = values.find(key);
  return it == values.end() ? fallback : it->second;
}

bool getFlag(const KeyValues& values, const char* key, bool fallback) {
  const auto it = values.find(key);
  if (it == values.end()) return fallback;
  const std::wstring& value = it->second;
  return value == L"1" || value == L"true" || value == L"yes" || value == L"on";
}

uint64_t getNumber(const KeyValues& values, const char* key, uint64_t fallback) {
  const auto it = values.find(key);
  if (it == values.end()) return fallback;
  const std::wstring& value = it->second;
  if (value.empty()) return fallback;
  wchar_t* end = nullptr;
  const unsigned long long parsed = std::wcstoull(value.c_str(), &end, 10);
  return end == value.c_str() ? fallback : static_cast<uint64_t>(parsed);
}

void appendLine(std::string& out, const std::string& name, const std::wstring& value) {
  if (value.empty()) return;  // 空字段不写，清单保持精简
  out += name;
  out += '=';
  out += toUtf8(value);
  out += "\n";
}

}  // namespace

bool parseManifest(const std::wstring& text, Manifest& out, std::wstring& error) {
  KeyValues values;
  if (!parseIni(text, values, error)) return false;

  Manifest manifest;
  manifest.displayName = getText(values, "app.displayname", {});
  manifest.version = getText(values, "app.version", {});
  manifest.publisher = getText(values, "app.publisher", {});
  manifest.exeName = getText(values, "app.exename", {});
  manifest.installSubdir = getText(values, "app.installsubdir", {});

  manifest.windowTitle = getText(values, "ui.windowtitle", {});
  manifest.title = getText(values, "ui.title", {});
  manifest.description = getText(values, "ui.description", {});
  manifest.logo = getText(values, "ui.logo", {});
  manifest.installText = getText(values, "ui.installtext", L"安装");
  manifest.cancelText = getText(values, "ui.canceltext", L"取消");
  manifest.browseText = getText(values, "ui.browsetext", L"浏览...");
  manifest.allUsersText = getText(values, "ui.alluserstext", L"所有用户");
  manifest.currentUserText = getText(values, "ui.currentusertext", L"仅当前用户");
  manifest.showScope = getFlag(values, "ui.showscope", true);
  manifest.runAfterText = getText(values, "ui.runaftertext", L"立即打开");
  manifest.runAfterChecked = getFlag(values, "ui.runafterchecked", true);

  manifest.configModule = getText(values, "config.module", {});
  manifest.afterInstall = getText(values, "run.afterinstall", {});

  // [commands] 段里的 cmd.<n>.path / .args / .text / .wait / .required：
  // 与文件表同样按下标归组，顺序与清单里的书写顺序无关。
  {
    constexpr const char* kCommandPrefix = "commands.cmd.";
    std::map<int, ManifestCommand> order;
    for (const auto& [key, value] : values) {
      if (key.rfind(kCommandPrefix, 0) != 0) continue;
      const size_t prefixSize = std::strlen(kCommandPrefix);
      const size_t dot = key.find('.', prefixSize);
      if (dot == std::string::npos) continue;

      const std::string indexText = key.substr(prefixSize, dot - prefixSize);
      if (indexText.empty()) continue;
      int index = 0;
      try {
        index = std::stoi(indexText);
      } catch (...) {
        continue;
      }

      const std::string field = key.substr(dot + 1);
      ManifestCommand& command = order[index];
      if (field == "path") {
        command.path = value;
      } else if (field == "args") {
        command.args = value;
      } else if (field == "text") {
        command.text = value;
      } else if (field == "wait") {
        command.wait = getFlag(values, key.c_str(), true);
      } else if (field == "required") {
        command.required = getFlag(values, key.c_str(), true);
      }
    }
    for (auto& [index, command] : order) {
      if (command.path.empty()) continue;  // 只有文案的残项跳过
      manifest.commands.push_back(command);
    }
  }

  if (manifest.displayName.empty()) {
    error = L"清单缺少 app.displayName";
    return false;
  }
  if (manifest.exeName.empty()) {
    error = L"清单缺少 app.exeName";
    return false;
  }

  // [files] 段里的 file.<n>.path / .offset / .size / .install：按下标归组，
  // 顺序与清单无关。
  //
  // 注意键在解析后是「段.键」的形式（files.file.0.path），段名前缀必须一起剥掉，
  // 否则永远匹配不上，文件表会是空的。
  constexpr const char* kFilePrefix = "files.file.";
  std::map<int, ManifestFile> byIndex;
  for (const auto& [key, value] : values) {
    if (key.rfind(kFilePrefix, 0) != 0) continue;
    const size_t prefixSize = std::strlen(kFilePrefix);

    const size_t dot = key.find('.', prefixSize);
    if (dot == std::string::npos) continue;

    const std::string indexText = key.substr(prefixSize, dot - prefixSize);
    if (indexText.empty()) continue;
    int index = 0;
    try {
      index = std::stoi(indexText);
    } catch (...) {
      continue;
    }

    const std::string field = key.substr(dot + 1);
    ManifestFile& file = byIndex[index];
    if (field == "path") {
      file.path = value;
    } else if (field == "offset") {
      file.offset = getNumber(values, key.c_str(), 0);
    } else if (field == "size") {
      file.size = getNumber(values, key.c_str(), 0);
    } else if (field == "install") {
      file.install = value != L"0" && value != L"false" && value != L"no";
    }
  }

  for (auto& [index, file] : byIndex) {
    if (file.path.empty()) continue;  // 只有 offset/size 的残项跳过
    manifest.files.push_back(file);
  }

  out = std::move(manifest);
  return true;
}

std::wstring formatManifest(const Manifest& manifest) {
  std::string text;
  text += "; 由 aiinstaller_pack 生成，请勿手工编辑\n";
  text += "; 源清单见应用工程里的 setup.ini\n\n";

  text += "[app]\n";
  appendLine(text, "displayName", manifest.displayName);
  appendLine(text, "version", manifest.version);
  appendLine(text, "publisher", manifest.publisher);
  appendLine(text, "exeName", manifest.exeName);
  appendLine(text, "installSubdir", manifest.installSubdir);
  text += "\n";

  text += "[ui]\n";
  appendLine(text, "windowTitle", manifest.windowTitle);
  appendLine(text, "title", manifest.title);
  appendLine(text, "description", manifest.description);
  appendLine(text, "logo", manifest.logo);
  appendLine(text, "installText", manifest.installText);
  appendLine(text, "cancelText", manifest.cancelText);
  appendLine(text, "browseText", manifest.browseText);
  appendLine(text, "allUsersText", manifest.allUsersText);
  appendLine(text, "currentUserText", manifest.currentUserText);
  appendLine(text, "showScope", manifest.showScope ? L"1" : L"0");
  if (manifest.runAfterText != L"立即打开") {
    appendLine(text, "runAfterText", manifest.runAfterText);
  }
  if (!manifest.runAfterChecked) text += "runAfterChecked=0\n";
  text += "\n";

  if (!manifest.configModule.empty()) {
    text += "[config]\n";
    appendLine(text, "module", manifest.configModule);
    text += "\n";
  }

  if (!manifest.afterInstall.empty()) {
    text += "[run]\n";
    appendLine(text, "afterInstall", manifest.afterInstall);
    text += "\n";
  }

  if (!manifest.commands.empty()) {
    text += "[commands]\n";
    for (size_t i = 0; i < manifest.commands.size(); ++i) {
      const ManifestCommand& command = manifest.commands[i];
      const std::string prefix = "cmd." + std::to_string(i) + ".";
      appendLine(text, prefix + "path", command.path);
      appendLine(text, prefix + "args", command.args);
      appendLine(text, prefix + "text", command.text);
      if (!command.wait) text += prefix + "wait=0\n";
      if (!command.required) text += prefix + "required=0\n";
    }
    text += "\n";
  }

  text += "[files]\n";
  for (size_t i = 0; i < manifest.files.size(); ++i) {
    const ManifestFile& file = manifest.files[i];
    const std::string prefix = "file." + std::to_string(i) + ".";
    appendLine(text, prefix + "path", file.path);
    text += prefix + "offset=" + std::to_string(file.offset) + "\n";
    text += prefix + "size=" + std::to_string(file.size) + "\n";
    if (!file.install) text += prefix + "install=0\n";
  }

  return fromUtf8(text);
}

}  // namespace AiInstaller
