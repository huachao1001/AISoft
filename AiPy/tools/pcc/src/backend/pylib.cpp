// `.pylib` 的实现。归档格式见头文件。
#include "pcc/backend/pylib.h"

#include "pcc/backend/fsutil.h"
#include "pcc/backend/json.h"

namespace pcc {
namespace {

// 支持的 manifest 格式版本。将来改结构时递增，好让旧打包器明确报错而不是
// 按旧字段拼出一条错的链接命令。
constexpr int kManifestFormatVersion = 1;

}  // namespace

bool parseManifest(const std::string& text, PylibManifest* manifest,
                   std::string* error) {
  JsonValue root;
  if (!parseJson(text, &root) || !root.isObject()) {
    *error = "manifest.json is not a JSON object";
    return false;
  }

  manifest->module = root.getString("module");
  if (manifest->module.empty()) {
    *error = "manifest.json has no 'module' field";
    return false;
  }

  manifest->formatVersion = root.getInt("formatVersion", 0);
  if (manifest->formatVersion != kManifestFormatVersion) {
    // 版本不认识时停下：宁可报错，也不要拿半懂的字段继续。
    *error = "manifest format version " +
             std::to_string(manifest->formatVersion) +
             " is not supported (expected " +
             std::to_string(kManifestFormatVersion) + ")";
    return false;
  }

  manifest->nativeLibrary = root.getString("nativeLibrary");
  manifest->bytecode = root.getString("bytecode");
  manifest->functions = jsonStringArray(root, "functions");
  manifest->bareNames = jsonStringArray(root, "bare_names");

  // platforms 是一个对象（平台键 → 归档内路径）。不用 map<string,string>
  // 取字段：我们的 JsonValue 的字段是 JsonValue，逐个取才拿得到值。
  if (const JsonValue* table = root.get("platforms")) {
    if (table->isObject()) {
      for (const auto& entry : table->fields) {
        if (entry.second.isString()) {
          manifest->platforms.emplace_back(entry.first, entry.second.text);
        }
      }
    }
  }

  // platform_files：平台 → {角色 → 归档内路径}。core.pylib 这种「一个平台
  // 带好几个库」的包用它，读取端按角色取（runtime / base / launcher），
  // 不按文件名猜。
  if (const JsonValue* table = root.get("platform_files")) {
    if (table->isObject()) {
      for (const auto& platform : table->fields) {
        if (!platform.second.isObject()) continue;
        std::vector<std::pair<std::string, std::string>> files;
        for (const auto& role : platform.second.fields) {
          if (role.second.isString()) {
            files.emplace_back(role.first, role.second.text);
          }
        }
        manifest->platformFiles.emplace_back(platform.first, std::move(files));
      }
    }
  }
  return true;
}

std::string hostPlatformKey() {
#if defined(_WIN32)
  // 目前只认真实支持的这一种：x64 的 Windows。ARM64 / x86 还没有构建目标，
  // 宁可返回一个不会匹配任何包的名字，也不要猜一个能匹配上但链接会失败的。
#if defined(_M_X64)
  return "win-x64";
#else
  return "win-unknown";
#endif
#elif defined(__linux__)
  return "linux-x64";
#else
  return "unknown";
#endif
}

bool Pylib::open(const std::string& path, std::string* error) {
  path_ = path;
  if (!openZipFile(path, &archive_, error)) return false;

  std::vector<std::uint8_t> text;
  if (!archive_.readFile("manifest.json", &text, error)) return false;
  if (!parseManifest(std::string(text.begin(), text.end()), &manifest_,
                     error)) {
    *error = path + ": " + *error;
    return false;
  }

  // ── 文件名即身份 ──
  //
  // 归档文件名必须与 manifest 的 module 字段完全一致。不校验的后果实测过：
  // 一个名叫 `fs.pylib`、里面写着 `module: "os"` 的包会被当作 fs 链进去，
  // 而它注册的是 os。症状是「链接信息显示一切正常，运行时却报扩展函数
  // 没有链接进来」——那条错误会把排查引向按需链接，而真正的问题是包名
  // 与内容对不上。
  //
  // 为什么用文件名当唯一身份：脚本里 `import fs` 找的就是 `fs.pylib`，
  // 那是用户能看见的唯一名字。让 manifest 再声明一个不同的名字，等于允许
  // 「包叫什么」有两个答案。
  const std::string stem = stripExtension(baseName(path));
  if (stem != manifest_.module) {
    *error = path + ": this archive is named '" + stem +
             "' but its manifest declares module '" + manifest_.module +
             "'; the file name must match the module name";
    return false;
  }
  return true;
}

std::string Pylib::libraryForHost() const {
  if (!manifest_.platforms.empty()) {
    const std::string wanted = hostPlatformKey();
    for (const auto& entry : manifest_.platforms) {
      if (entry.first == wanted) return entry.second;
    }
    // 有平台表但本机不在其中：返回空串，让 extractLibrary 报「不支持本平台」。
    return std::string();
  }
  return manifest_.nativeLibrary;
}

bool Pylib::extractLibrary(const std::string& outputPath, std::uint64_t* size,
                           std::string* error) const {
  const std::string library = libraryForHost();
  if (library.empty()) {
    if (!manifest_.platforms.empty()) {
      std::string available;
      for (const auto& entry : manifest_.platforms) {
        if (!available.empty()) available += ", ";
        available += entry.first;
      }
      *error = path_ + ": this module has no native library for " +
               hostPlatformKey() + " (available: " + available + ")";
    } else {
      *error = path_ + ": manifest has no 'nativeLibrary'";
    }
    return false;
  }
  std::vector<std::uint8_t> data;
  if (!archive_.readFile(library, &data, error)) {
    *error = path_ + ": " + *error;
    return false;
  }
  if (!writeFileBytes(outputPath, data)) {
    *error = "cannot write " + outputPath;
    return false;
  }
  if (size != nullptr) *size = data.size();
  return true;
}

std::vector<std::pair<std::string, std::string>> Pylib::filesForHost() const {
  const std::string wanted = hostPlatformKey();
  std::vector<std::pair<std::string, std::string>> files;

  // 多文件包优先：它才表达得了「一个平台好几个库」。
  for (const auto& platform : manifest_.platformFiles) {
    if (platform.first != wanted) continue;
    return platform.second;
  }
  // 单文件包：角色就用模块名。调用方只取那一个。
  for (const auto& entry : manifest_.platforms) {
    if (entry.first != wanted) continue;
    files.emplace_back(manifest_.module, entry.second);
    return files;
  }
  if (!manifest_.nativeLibrary.empty()) {
    files.emplace_back(manifest_.module, manifest_.nativeLibrary);
  }
  return files;
}

bool Pylib::extractFile(const std::string& role, const std::string& outputPath,
                        std::uint64_t* size, std::string* error) const {
  const std::vector<std::pair<std::string, std::string>> files = filesForHost();
  if (files.empty()) {
    *error = path_ + ": this module has no files for " + hostPlatformKey();
    return false;
  }
  std::string stored;
  std::string available;
  for (const auto& entry : files) {
    if (!available.empty()) available += ", ";
    available += entry.first;
    if (entry.first == role) stored = entry.second;
  }
  if (stored.empty()) {
    *error = path_ + ": this archive has no '" + role + "' for " +
             hostPlatformKey() + " (available: " + available + ")";
    return false;
  }
  std::vector<std::uint8_t> data;
  if (!archive_.readFile(stored, &data, error)) {
    *error = path_ + ": " + *error;
    return false;
  }
  if (!writeFileBytes(outputPath, data)) {
    *error = "cannot write " + outputPath;
    return false;
  }
  if (size != nullptr) *size = data.size();
  return true;
}

bool Pylib::readBytecode(std::vector<std::uint8_t>* out,
                         std::string* error) const {
  if (manifest_.bytecode.empty()) {
    *error = path_ + ": this module has no Python layer (no 'bytecode')";
    return false;
  }
  if (!archive_.readFile(manifest_.bytecode, out, error)) {
    *error = path_ + ": " + *error;
    return false;
  }
  return true;
}

std::string findPylibArchive(const std::string& directory,
                             const std::string& module) {
  const std::string name = module + ".pylib";
  const std::string direct = joinPath(directory, name);
  if (fileExists(direct)) return direct;
  const std::string nested = joinPath(joinPath(directory, "lib"), name);
  if (fileExists(nested)) return nested;
  return std::string();
}

}  // namespace pcc
