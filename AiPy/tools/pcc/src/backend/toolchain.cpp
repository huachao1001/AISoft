// 平台层的实现。
#include "pcc/backend/toolchain.h"

#include "pcc/backend/coff.h"
#include "pcc/backend/fsutil.h"
#include "pcc/backend/process.h"
#include "pcc/diagnostics.h"

#include <algorithm>
#include <cstdlib>

namespace pcc {
namespace {

// 模块名 → 符号前缀：点换成下划线（`os.path` → `os_path`）。
//
// C++ 标识符里不能有点，而对外模块名必须带点。两边用同一条规则换算，
// 打包器（读 COFF 符号）与链接器（拼 /INCLUDE）才不会各写一套。
std::string symbolPrefix(const std::string& module) {
  std::string prefix = module;
  for (char& ch : prefix) {
    if (ch == '.') ch = '_';
  }
  return prefix;
}

}  // namespace

MsvcToolchain::MsvcToolchain(std::string linkExe,
                             std::vector<std::string> libraryPaths)
    : linkExe_(std::move(linkExe)), libraryPaths_(std::move(libraryPaths)) {}

std::string MsvcToolchain::symbolFor(const std::string& module,
                                     const std::string& function) const {
  return "aipy_" + symbolPrefix(module) + "_fn_" + function;
}

std::string MsvcToolchain::moduleSymbol(const std::string& module) const {
  return "aipy_pylib_entry_" + symbolPrefix(module);
}

std::string MsvcToolchain::bareSymbol(const std::string& module,
                                      const std::string& bare) const {
  return "aipy_" + symbolPrefix(module) + "_bare_" + bare;
}

std::string MsvcToolchain::quoteInclude(const std::string& symbol) const {
  return "/INCLUDE:" + symbol;
}

std::string MsvcToolchain::outputFlag(const std::string& path) const {
  return "/OUT:" + path;
}

std::vector<std::string> MsvcToolchain::subsystemFlags(Subsystem kind) const {
  if (kind == Subsystem::Gui) {
    // `/ENTRY` 必须显式给：`/subsystem:windows` 让链接器默认去找 `WinMain`，
    // 而 launcher 只有 `main`。CRT 的 `mainCRTStartup` 才是那个会调 `main`
    // 的入口，两个参数缺一不可（实测过单独给 `/subsystem:windows` 会报
    // 「unresolved external symbol WinMain」）。
    return {"/subsystem:windows", "/ENTRY:mainCRTStartup"};
  }
  return {"/subsystem:console"};
}

std::vector<std::string> MsvcToolchain::systemLibraries() const {
  // MSVC 的默认导入库。CRT 由 `/MT` 静态链进来，不在这里。
  return {"kernel32.lib",  "user32.lib", "gdi32.lib", "winspool.lib",
          "shell32.lib",   "ole32.lib",  "oleaut32.lib", "uuid.lib",
          "comdlg32.lib",  "advapi32.lib"};
}

std::vector<std::string> MsvcToolchain::libraryNames(
    const std::string& path) const {
  // 读库自己的符号索引。为什么不解析 `link.exe /dump`：
  //
  // 当前 MSVC 的 link.exe 已经把 /dump 移除（实测报 LNK1561 之后直接
  // 走链接流程），所以那条路永远拿不到符号，而失败会静默退化成
  // 「没有函数级符号 → 整库链接」。整库链接不会报错，只是产物变大——
  // 这正是最该避免的静默退化。
  std::vector<LibrarySymbol> symbols;
  std::string error;
  if (!readLibrarySymbols(path, &symbols, &error)) return {};

  std::vector<std::string> names;
  names.reserve(symbols.size());
  for (const LibrarySymbol& symbol : symbols) names.push_back(symbol.name);
  return names;
}

Toolchain* makeToolchain(std::string* linkExe,
                         const std::vector<std::string>& libraryPaths,
                         std::string* error) {
#ifdef _WIN32
  (void)error;
  if (linkExe->empty()) *linkExe = findLinker(error);
  if (linkExe->empty()) return nullptr;
  return new MsvcToolchain(*linkExe, libraryPaths);
#else
  (void)linkExe;
  (void)libraryPaths;
  *error = diag::unsupportedPlatform("non-Windows");
  return nullptr;
#endif
}

namespace {

// 在 PATH 里找 link.exe。找到返回绝对路径，否则空串。
std::string linkFromPath() {
  const char* raw = std::getenv("PATH");
  if (raw == nullptr) return std::string();
  std::string path = raw;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find(';', start);
    std::string part = path.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    // 去掉引号：PATH 里带空格的项常常写成 "C:\Program Files\..."
    if (part.size() >= 2 && part.front() == '"' && part.back() == '"') {
      part = part.substr(1, part.size() - 2);
    }
    if (!part.empty()) {
      const std::string candidate = joinPath(part, "link.exe");
      if (fileExists(candidate)) return candidate;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return std::string();
}

// 用 vswhere 问「已装的 VS / BuildTools 里哪个 link.exe 最新」。
//
// vswhere 是微软官方给的做法，位置固定在 Program Files (x86)\Microsoft
// Visual Studio\Installer 下（BuildTools 也共用这个安装器）。它认得出
// 各种安装布局，比自己拼版本号目录可靠——版本号目录的深度和命名都变过。
std::string linkFromVsWhere() {
  const char* probes[] = {
      "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe",
      "C:\\Program Files\\Microsoft Visual Studio\\Installer\\vswhere.exe",
  };
  std::string vswhere;
  for (const char* probe : probes) {
    if (fileExists(probe)) {
      vswhere = probe;
      break;
    }
  }
  if (vswhere.empty()) return std::string();

  std::vector<std::string> command = {
      vswhere, "-latest", "-products", "*", "-find",
      "VC\\Tools\\MSVC\\**\\bin\\Hostx64\\x64\\link.exe"};
  const ProcessResult completed = runProcess(command, {});
  if (!completed.started || completed.exitCode != 0) return std::string();

  // 输出可能有 CRLF 与空行；取第一个存在的路径。
  std::string text = completed.out;
  std::string line;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    const char ch = i < text.size() ? text[i] : '\n';
    if (ch == '\r') continue;
    if (ch != '\n') {
      line.push_back(ch);
      continue;
    }
    if (!line.empty() && fileExists(line)) return line;
    line.clear();
  }
  return std::string();
}

}  // namespace

std::string findLinker(std::string* error) {
  const std::string fromPath = linkFromPath();
  if (!fromPath.empty()) return fromPath;

  const std::string fromVs = linkFromVsWhere();
  if (!fromVs.empty()) return fromVs;

  if (error != nullptr && error->empty()) {
    *error =
        "cannot find link.exe. Install the Visual Studio Build Tools (the "
        "linker is all that is needed), or run this from a Developer Prompt "
        "so that link.exe is on PATH";
  }
  return std::string();
}

std::vector<std::string> libraryPathsFor(const std::string& linkExe) {
  std::vector<std::string> paths;

  // MSVC 库目录：逐级上溯找 `lib/x64/libcpmt.lib`。
  std::string probe = dirName(absolutePath(linkExe));
  for (int depth = 0; depth < 8 && !probe.empty(); ++depth) {
    const std::string candidate = joinPath(joinPath(probe, "lib"), "x64");
    if (fileExists(joinPath(candidate, "libcpmt.lib"))) {
      paths.push_back(candidate);
      break;
    }
    const std::string parent = dirName(probe);
    if (parent == probe) break;
    probe = parent;
  }

  // Windows SDK：取版本号最大的那个。
  const char* roots[] = {
      "C:\\Program Files (x86)\\Windows Kits\\10\\Lib",
      "C:\\Program Files\\Windows Kits\\10\\Lib",
  };
  for (const char* root : roots) {
    if (!directoryExists(root)) continue;
    std::vector<std::string> versions;
    for (const std::string& name : listDirectory(root)) {
      if (directoryExists(joinPath(root, name))) versions.push_back(name);
    }
    if (versions.empty()) continue;
    std::sort(versions.begin(), versions.end());
    const std::string base = joinPath(root, versions.back());
    for (const char* part : {"ucrt\\x64", "um\\x64"}) {
      const std::string candidate = joinPath(base, part);
      if (directoryExists(candidate)) paths.push_back(candidate);
    }
  }
  return paths;
}

}  // namespace pcc
