// `os.listdir(path)` —— 目录下的名字列表（不含 "." / ".."）。
//
// 对齐 CPython 的 `os.listdir`：返回 list，顺序是文件系统的原始顺序
// （**不排序**），要稳定就先 `sorted(...)`。
//
// 失败返回 None。目录不存在与「不是目录」都算失败。
//
// 与 CPython 的差异只有一处，且由语言决定：CPython 抛 FileNotFoundError，
// 这里返回 None——AiPy 没有异常通道（见 vm.h 文件头）。
#include "aipy/io/paths.h"
#include "aipy/pylib.h"

#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace {

#if defined(_WIN32)

// 宽字符转 UTF-8。目录名里可能有中文，直接按窄字符读会变成乱码。
std::string toUtf8(const wchar_t* wide) {
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0,
                                         nullptr, nullptr);
  if (needed <= 1) return std::string();
  std::string narrow(static_cast<std::size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, &narrow[0], needed, nullptr, nullptr);
  return narrow;
}

bool listDirectory(const std::string& path, std::vector<std::string>* names) {
  const aipy::io::NativePath wide = aipy::io::toNativePath(path);
  if (wide.empty()) return false;
  // `\*`：`FindFirstFileW` 要求以通配符结尾，否则只匹配这个名字本身。
  std::wstring pattern = wide;
  if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
    pattern += L'\\';
  }
  pattern += L'*';

  WIN32_FIND_DATAW found;
  HANDLE handle = FindFirstFileW(pattern.c_str(), &found);
  if (handle == INVALID_HANDLE_VALUE) return false;

  do {
    // "." / ".." 不是子项，Python 的 listdir 也不返回它们。
    if (found.cFileName[0] == L'.' &&
        (found.cFileName[1] == L'\0' ||
         (found.cFileName[1] == L'.' && found.cFileName[2] == L'\0'))) {
      continue;
    }
    std::string name = toUtf8(found.cFileName);
    if (!name.empty()) names->push_back(std::move(name));
  } while (FindNextFileW(handle, &found) != 0);

  FindClose(handle);
  return true;
}

#else

bool listDirectory(const std::string& path, std::vector<std::string>* names) {
  DIR* directory = opendir(path.c_str());
  if (directory == nullptr) return false;
  while (struct dirent* entry = readdir(directory)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    names->push_back(name);
  }
  closedir(directory);
  return true;
}

#endif

aipy::Value listdirImpl(const aipy::Value* args, int count) {
  if (count < 1) return aipy::Value();
  const std::string path = args[0].toString();
  std::vector<std::string> names;
  if (!listDirectory(path, &names)) {
    aipy::diagnose(aipy::io::describeError(path, "无法列出目录"));
    return aipy::Value();
  }
  aipy::Value::List items;
  items.reserve(names.size());
  for (const std::string& name : names) items.push_back(aipy::Value(name));
  return aipy::Value::list(std::move(items));
}

}  // namespace

AIPY_PYLIB_FUNCTION(os, listdir, listdirImpl);
