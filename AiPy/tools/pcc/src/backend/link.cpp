// 按需链接的实现。
//
// ── 为什么不硬编码 link.exe 与库路径 ──
//
// 那些路径随 MSVC 版本、随安装位置变。所以从 CMake 的构建目录读
// `CMakeCache.txt`（CMAKE_LINKER）与 `build.ninja`（launcher 的 obj 与库
// 清单）；或者读 dist/ 里构建期录好的配方。
#include "pcc/backend/link.h"

#include "pcc/backend/fsutil.h"
#include "pcc/backend/json.h"
#include "pcc/backend/process.h"
#include "pcc/backend/pylib.h"
#include "pcc/backend/toolchain.h"
#include "pcc/diagnostics.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>

namespace pcc {
namespace {

// launcher 的入口符号。重链接时入口来自 `lib/launcher.lib`，而库里没人
// 引用的 obj 不会被抽出，必须显式点名——否则链接器报「未指定对象文件」
// 加「mainCRTStartup 无法解析」。
constexpr char kLauncherEntrySymbol[] = "main";

}  // namespace

// 扫 libraryDir 里所有 `.pylib`，找出声明了某个裸名的模块。
//
// ── 为什么必须扫目录 ──
//
// 裸名（`open`）不带模块前缀，所以「谁提供 open」只能问库自己。`.pylib`
// 的 manifest 里记着 bareNames，扫描它比解包读符号表便宜得多，也不必把
// 「哪个符号是裸名」这条规则复制到链接器里。
//
// ── 多个库声明同一个裸名就报错 ──
//
// 静默取第一个是最坏的处理：谁生效取决于文件名排序，同一次构建里换个
// 文件名结果就变了，而且不报错。裸名是**全局名字**（脚本里只写 `open`），
// 两个库都想占用它，必须是使用者解决的冲突，不是打包器替他选。
std::string findBareProvider(const std::string& directory,
                            const std::string& bare,
                            std::string* module, std::string* error) {
  // 两个位置都找：direct 是仓库/构建目录，`lib/` 是 dist 的布局。
  // 与 findPylibArchive 同一条约定，否则「构建能跑、dist 不能跑」。
  const std::string directories[2] = {directory, joinPath(directory, "lib")};
  const std::string suffix = ".pylib";
  std::string found;
  std::vector<std::string> contenders;
  for (const std::string& current : directories) {
    std::vector<std::string> names = listDirectory(current);
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
      if (name.size() <= suffix.size()) continue;
      if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) !=
          0) {
        continue;
      }
      const std::string path = joinPath(current, name);
      Pylib pylib;
      if (!pylib.open(path, error)) return std::string();
      for (const std::string& provided : pylib.manifest().bareNames) {
        if (provided != bare) continue;
        if (found.empty()) {
          found = path;
          *module = pylib.manifest().module;
        } else {
          contenders.push_back(pylib.manifest().module);
        }
      }
    }
  }
  if (!contenders.empty()) {
    std::string list = *module;
    for (const std::string& name : contenders) list += ", " + name;
    *error = "the bare name '" + bare + "' is declared by more than one " +
             "library (" + list +
             "); a bare name is a global name, so only one library may "
             "provide it";
    return std::string();
  }
  return found;
}

std::vector<ModuleFunctions> groupExtensions(
    const std::vector<std::string>& qualifiedNames,
    const std::vector<std::string>& knownModules) {
  std::vector<ModuleFunctions> grouped;
  for (const std::string& qualified : qualifiedNames) {
    // 模块名可能是点分的（`os.path.exists`）。不能按第一个点切：那样会把
    // 模块读成 `os`、函数读成 `path.exists`。
    //
    // 做法是拿「已知模块名」去匹配最长前缀。为什么需要已知清单：只凭名字
    // 本身无法判断 `os.path.exists` 是「模块 os.path 的函数 exists」还是
    // 「模块 os 的函数 path.exists」——前半段只有库里才知道。
    //
    // 匹配不上任何已知模块时退回第一个点：这样报错能落在
    // 「找不到 os 这个库」而不是「名字格式不对」，对使用者更有用。
    std::string module;
    std::string function;
    std::size_t best = 0;
    for (const std::string& candidate : knownModules) {
      if (candidate.size() <= best) continue;
      if (qualified.size() <= candidate.size()) continue;
      if (qualified.compare(0, candidate.size(), candidate) != 0) continue;
      if (qualified[candidate.size()] != '.') continue;
      best = candidate.size();
      module = candidate;
    }
    if (!module.empty()) {
      function = qualified.substr(module.size() + 1);
    } else {
      const std::size_t dot = qualified.find('.');
      if (dot == std::string::npos) continue;
      module = qualified.substr(0, dot);
      function = qualified.substr(dot + 1);
    }
    if (module.empty() || function.empty()) continue;

    bool found = false;
    for (ModuleFunctions& entry : grouped) {
      if (entry.module == module) {
        entry.functions.push_back(function);
        found = true;
        break;
      }
    }
    if (!found) grouped.push_back(ModuleFunctions{module, {function}});
  }
  return grouped;
}

bool relink(const std::string& buildDir, const std::string& libraryDir,
            const std::vector<std::string>& modules,
            const std::string& output,
            const std::vector<std::string>& extensions, LinkResult* result,
            std::string* error, const std::string& scratchDir,
            Subsystem subsystem) {
  std::string linkExe;
  std::vector<std::string> objects;
  std::vector<std::string> libraries;
  std::string baseDir;
  std::string runtimeDir;
  std::string entry;

  // 链接器：两种模式都由 toolchain 自己找（PATH → vswhere）。
  //
  // 不再读任何配置文件。理由见 include/pcc/backend/toolchain.h 的 findLinker：
  // 链接器的位置是**运行打包的这台机器**的事实，而 dist/ 是从构建机拷来的。
  // 把构建机的绝对路径带在 dist 里，换台机器就失效，而对方明明装着链接器。
  linkExe = findLinker(error);
  if (linkExe.empty()) return false;

  // 本机的 MSVC / SDK 库路径：dist 只带语言运行时与 .pylib，系统导入库与
  // CRT 仍来自链接器套件。
  const std::vector<std::string> libraryPaths = libraryPathsFor(linkExe);
  std::unique_ptr<Toolchain> toolchain(
      makeToolchain(&linkExe, libraryPaths, error));
  if (!toolchain) return false;

  // ── 两种模式靠「有没有 core.pylib」区分 ──
  //
  // dist：`--lib-dir` 里有 `lib/core.pylib`。语言运行时从归档里解出来，
  //       入口也是归档里的 `launcher.lib`。
  // 构建：没有 core.pylib。运行时与入口都从构建目录拿。
  //
  // 判据不是「有没有某个配置文件」——那个配置文件曾经存在过（记录构建机
  // 的链接器路径与库清单），后来发现它三项里两项是**固定约定**、一项是
  // **构建机的事实**，没有一项该由发布目录携带。删掉它之后 dist 是纯的
  // 「二进制 + 归档」，拷到任何机器都能用。
  const std::string corePath = joinPath(joinPath(libraryDir, "lib"),
                                        "core.pylib");
  const bool distMode = fileExists(corePath);

  if (distMode) {
    baseDir = libraryDir;

    // 语言运行时三件套必须一起解到同一目录：命令行上它们是并列的库，
    // 而且入口也在里面。角色名是固定契约（见 pack_pylib.py 与 CMakeLists）。
    Pylib core;
    if (!core.open(corePath, error)) return false;
    struct Role {
      const char* role;
      const char* file;
    };
    const Role roles[3] = {{"runtime", "aipy.lib"},
                           {"base", "aipy_base.lib"},
                           {"launcher", "launcher.lib"}};
    runtimeDir = scratchDir;
    for (const Role& item : roles) {
      const std::string target = joinPath(scratchDir, item.file);
      std::uint64_t size = 0;
      if (!core.extractFile(item.role, target, &size, error)) return false;
    }
    entry = "launcher.lib";

    // 系统库不在这里列：MSVC 编出的 .obj 自带 /DEFAULTLIB 指令，链接器
    // 会自己引入需要的库。实测（examples 四份含线程库）去掉清单后打包与
    // 运行都正常。留着清单等于把「这台机器的系统库名」固化进发布件，
    // 换平台就得重打一份 dist。
  } else {
    // 构建模式：运行时与入口都来自构建目录。
    //
    // 链接器不读构建目录里的 CMAKE_LINKER 了——它也来自那种「构建机事实」
    // 思路，而 toolchain 能自己找到本机的链接器。只保留 buildDir 用来找
    // 运行时库（那是构建产物，本机确实需要从那里拿）。
    baseDir = buildDir;
    runtimeDir = buildDir;

    // 入口库名问 toolchain（`.lib` / `.a` 是平台细节）。
    entry = toolchain->launcherLibraryName();
    if (!fileExists(joinPath(buildDir, entry))) {
      *error = "cannot find " + joinPath(buildDir, entry) +
               " (build once first, or pass --lib-dir pointing at dist/)";
      return false;
    }
  }

  // 系统库由 toolchain 给（平台细节）。dist 与构建两种模式一样——
  // 它们是「链接器所在平台」的事实，与 dist/ 无关。
  libraries = toolchain->systemLibraries();

  // ── 扩展库：只有 `.pylib` 一种形态 ──
  //
  // 不再接受裸 `.lib`。理由：库的「交付形态」与「构建期形态」如果是两种
  // 东西，那条裸 `.lib` 路径就永远不会被真实发布流程覆盖——它只会在测试里
  // 活着，而测试通过不代表 dist 能用。构建目录也产出 `.pylib`，于是
  // `--lib-dir build` 与 `--lib-dir dist` 走的是同一条代码路径。
  //
  // 解出来的原生库落在 scratchDir：链接器只接受文件路径，且扩展名必须符合
  // 本平台（Windows `.lib`、Linux/macOS `.a`），否则它不把它当库。
  std::vector<std::string> used;
  for (const std::string& module : modules) {
    const std::string archive = findPylibArchive(libraryDir, module);
    if (archive.empty()) {
      *error = diag::missingLibrary(module, libraryDir);
      return false;
    }
    Pylib pylib;
    if (!pylib.open(archive, error)) return false;
    const std::string target = joinPath(
        scratchDir, module + "_pylib" + toolchain->librarySuffix());
    std::uint64_t size = 0;
    if (!pylib.extractLibrary(target, &size, error)) return false;
    used.push_back(target);
  }
  result->libraries = used;

  std::vector<std::string> command = {
      linkExe, "/nologo", "/machine:x64",
      "/INCREMENTAL:NO", toolchain->outputFlag(output)};
  // 子系统由 toolchain 给：它是平台细节（MSVC 是 `/subsystem:` 加 `/ENTRY:`）。
  for (const std::string& flag : toolchain->subsystemFlags(subsystem)) {
    command.push_back(flag);
  }

  // 绝对化：link.exe 的工作目录是我们给的，相对路径会找不到。
  for (const std::string& object : objects) {
    const std::string full = joinPath(baseDir, object);
    if (!fileExists(full)) {
      *error = diag::missingObject(full);
      return false;
    }
    command.push_back(full);
  }

  // 入口来自静态库，要点名：库里的 `main` 没人引用，链接器不会去抽它，
  // 于是报「未指定对象文件」+「mainCRTStartup 无法解析」。这个坑实测踩过。
  //
  // 文件名由模式决定（dist 是 `launcher.lib`，构建是 toolchain 给的
  // `aipy_launcher.lib`），所以这里只负责拼路径与点名。
  if (!entry.empty()) {
    const std::string entryPath = joinPath(runtimeDir, entry);
    if (!fileExists(entryPath)) {
      *error = "cannot find the launcher entry library: " + entryPath +
               " (build once first, or pass --lib-dir pointing at dist/)";
      return false;
    }
    command.push_back(toolchain->quoteInclude(kLauncherEntrySymbol));
    command.push_back(entryPath);
  }

  // ── 语言运行时与系统库 ──
  //
  // 两种模式的来源不同：
  //   dist（v2 配方）  运行时从 core.pylib 解出来的两份，在 runtimeDir
  //   构建            运行时在构建目录，配方里以绝对路径列出
  //
  // 运行时**不能** whole-archive：那会把整个 aipy.lib 拖进来，产物从
  // 328 KB 涨到几百 KB。它按正常方式链接，缺什么抽什么。
  const std::vector<std::string> runtimeNames = {"aipy.lib", "aipy_base.lib"};
  for (const std::string& name : runtimeNames) {
    const std::string full = joinPath(runtimeDir, name);
    if (!fileExists(full)) {
      *error = diag::missingRuntime(name, runtimeDir);
      return false;
    }
    command.push_back(full);
  }

  for (const std::string& library : libraries) {
    // 系统导入库（kernel32 等）原样传，由 LIB 环境变量解析。
    //
    // 运行时若也出现在清单里就跳过：上面已经加过了，重复的库路径会让
    // 链接器报 LNK4006 重复符号。
    if (baseName(library) == "aipy.lib" ||
        baseName(library) == "aipy_base.lib") {
      continue;
    }
    command.push_back(library);
  }
  // ── 按需：逐个函数点名 ──
  //
  // 静态库里没人引用的 obj 不会被抽出，而注册项天生没人引用（它们靠节合并
  // 被发现）。所以「什么都不做」的结局是空产物——探针验证过：不点名时四个
  // 函数全部查不到。
  //
  // 函数清单**只信 manifest**。不解析链接器输出（当前 link.exe 已不认
  // `/dump`），也不解析 COFF/ELF 符号表反推——两条路都会在失败时静默退化
  // 成「整库链接」，而这正是最该避免的：产物变大却不报错。

  // ── 裸名（`open`）先解析成「模块.函数」──
  //
  // 裸名不带模块前缀，所以「谁提供它」只能问库自己：扫 --lib-dir 里所有
  // `.pylib`，找 manifest 里声明了 bareNames 的那个。解析出来后当成普通的
  // 扩展调用走后面的流程，链接器不必为裸名单开一条路径。
  //
  // 找不到就报错，且列出实际有哪些裸名可用——与「扩展函数没链接进来」
  // 同一个思路：说清「该有什么」，而不是只说「这个不行」。
  std::vector<std::string> resolved;
  std::set<std::string> bareNames;  // 脚本作为裸名用到的名字
  for (const std::string& name : extensions) {
    if (name.find('.') != std::string::npos) {
      resolved.push_back(name);
      continue;
    }
    std::string provider;
    const std::string archive =
        findBareProvider(libraryDir, name, &provider, error);
    if (archive.empty()) {
      if (!error->empty()) return false;
      *error = diag::unknownBareName(name, libraryDir);
      return false;
    }
    // 记录成 "fs.open"，后面按模块分组时它与 `fs.open(...)` 完全等价。
    // 对使用者来说这也是同一件事：`open` 就是 `fs.open` 的缩写。
    resolved.push_back(provider + "." + name);
    bareNames.insert(name);
  }

  // 已知模块名：用来判断点分模块名的边界（`os.path.exists` 里模块是
  // `os.path` 而不是 `os`）。从 --lib-dir 里的归档文件名反推——文件名
  // 就是模块名（`os.path.pylib`）。
  std::vector<std::string> knownModules;
  {
    const std::string directories[2] = {libraryDir,
                                        joinPath(libraryDir, "lib")};
    const std::string suffix = ".pylib";
    for (const std::string& current : directories) {
      for (const std::string& name : listDirectory(current)) {
        if (name.size() <= suffix.size()) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(),
                         suffix) != 0) {
          continue;
        }
        knownModules.push_back(name.substr(0, name.size() - suffix.size()));
      }
    }
  }
  const std::vector<ModuleFunctions> grouped =
      groupExtensions(resolved, knownModules);
  for (const ModuleFunctions& group : grouped) {
    const std::string archive = findPylibArchive(libraryDir, group.module);
    if (archive.empty()) {
      *error = diag::missingLibrary(group.module, libraryDir);
      return false;
    }
    Pylib pylib;
    if (!pylib.open(archive, error)) return false;
    // 库本身必须出现在命令行上，否则被点名的符号无从解析。
    const std::string libraryPath = joinPath(
        scratchDir, group.module + "_pylib" + toolchain->librarySuffix());
    std::uint64_t size = 0;
    if (!pylib.extractLibrary(libraryPath, &size, error)) return false;
    command.push_back(libraryPath);

    const std::vector<std::string>& provided = pylib.manifest().functions;
    auto provides = [&provided](const std::string& function) {
      for (const std::string& name : provided) {
        if (name == function) return true;
      }
      return false;
    };

    for (const std::string& function : group.functions) {
      // 脚本调了 manifest 里没有的函数：这是「库版本与脚本不匹配」，
      // 必须现在报错。放过去的结果是链接期报一个 /INCLUDE 的符号不存在，
      // 那条信息远不如这里直接说清楚是哪个模块缺哪个函数。
      if (!provides(function)) {
        *error = diag::missingFunction(group.module, function, archive,
                                       provided);
        return false;
      }
      const std::string symbol = toolchain->symbolFor(group.module, function);
      command.push_back(toolchain->quoteInclude(symbol));
      result->symbols.push_back(symbol);
      result->functions.push_back(group.module + "." + function);
    }

    // 模块级注册项也要点名，否则 VM 认不出这个模块名。
    const std::string moduleSymbol = toolchain->moduleSymbol(group.module);
    command.push_back(toolchain->quoteInclude(moduleSymbol));
    result->symbols.push_back(moduleSymbol);

    // 裸名注册项：脚本用了 `open` 这种短名时，对应的 `.aipybare` 项必须
    // 进产物，否则运行时 resolveBare 扫不到它。
    //
    // 判据是「这个名字是不是作为裸名出现的」——只有裸名调用才需要它，
    // `fs.open(...)` 这种带前缀的写法不需要。
    for (const std::string& function : group.functions) {
      if (!bareNames.count(function)) continue;
      const std::string bareSymbol = toolchain->bareSymbol(group.module,
                                                            function);
      command.push_back(toolchain->quoteInclude(bareSymbol));
      result->symbols.push_back(bareSymbol);
    }
  }

  std::vector<std::string> environment;
  if (!libraryPaths.empty()) {
    std::string joined;
    for (const std::string& part : libraryPaths) {
      if (!joined.empty()) joined.push_back(';');
      joined += part;
    }
    const char* inherited = std::getenv("LIB");
    if (inherited != nullptr && *inherited != '\0') joined += ";";
    if (inherited != nullptr) joined += inherited;
    environment.push_back("LIB=" + joined);
  }

  const ProcessResult completed = runProcess(command, environment);
  if (!completed.started) {
    *error = "cannot start the linker: " + linkExe + " (Win32 error " +
             std::to_string(completed.lastError) + ")";
    return false;
  }
  if (completed.exitCode != 0) {
    *error = diag::linkFailed(completed.out, completed.err);
    return false;
  }
  return true;
}

}  // namespace pcc
