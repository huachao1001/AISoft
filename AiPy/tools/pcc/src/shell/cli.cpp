// CLI 的实现。
#include "pcc/shell/cli.h"

#include "pcc/backend/bundle.h"
#include "pcc/backend/fsutil.h"
#include "pcc/backend/link.h"
#include "pcc/backend/pack.h"
#include "pcc/backend/peimage.h"
#include "pcc/backend/pylib.h"
#include "pcc/codegen/codegen.h"
#include "pcc/diagnostics.h"
#include "pcc/frontend/lexer.h"
#include "pcc/frontend/parser.h"
#include "pcc/shell/disasm.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace pcc {
namespace {

// 编译一步到位：源码 → Program。错误带位置。
bool compileSource(const std::string& source, const std::string& filename,
                   const std::string& moduleName, Program* program,
                   std::string* error) {
  Lexer lexer(source, filename);
  if (!lexer.tokenize()) {
    const LexError& failure = lexer.error();
    *error = filename + ":" + std::to_string(failure.line) + ":" +
             std::to_string(failure.col + 1) + ": error: " + failure.message;
    return false;
  }

  Parser parser(lexer.tokens(), filename);
  NodePtr module = parser.parseModule();
  if (!module) {
    const ParseError& failure = parser.error();
    *error = filename + ":" + std::to_string(failure.line) + ":" +
             std::to_string(failure.col + 1) + ": error: " + failure.message;
    return false;
  }

  CodeGen codegen(filename, moduleName);
  if (!codegen.compile(*module)) {
    const CompileError& failure = codegen.error();
    *error = filename + ":" + std::to_string(failure.line) + ":" +
             std::to_string(failure.col) + ": error: " + failure.message;
    return false;
  }
  *program = codegen.program();
  return true;
}

// 模块名默认取文件名（不带扩展名），与 compile_file 一致。
std::string moduleNameOf(const std::string& path) {
  return baseName(stripExtension(path));
}

bool compileFile(const std::string& path, Program* program,
                 std::string* error) {
  std::string source;
  if (!readFileText(path, &source)) {
    *error = diag::fileNotFound(path);
    return false;
  }
  // 去 BOM：Windows 编辑器的「UTF-8 with BOM」会写 EF BB BF，
  // 带上它会让词法把首字符当成标识符字符。
  if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
      static_cast<unsigned char>(source[1]) == 0xBB &&
      static_cast<unsigned char>(source[2]) == 0xBF) {
    source = source.substr(3);
  }
  return compileSource(source, path, moduleNameOf(path), program, error);
}

// 从扩展表里取出模块名，去重、保持首次出现顺序。
//
// 按需链接的判据是**扩展表**（真的调用了）而不是 import 表（只是提到）：
// `import text` 但一个 text 函数都没用，不该把 text 的库链进去。
//
// 不带点号的名字是**裸名**（`open`），不是模块名：它由 relink 扫 --lib-dir
// 解析成「哪个库声明了它」。这里跳过，否则会去找一个叫 open.pylib 的库。
std::vector<std::string> extensionModules(const Program& program) {
  std::vector<std::string> modules;
  for (const std::string& qualified : program.extensions) {
    const std::size_t dot = qualified.find('.');
    if (dot == std::string::npos) continue;  // 裸名：交给 relink 解析
    const std::string module = qualified.substr(0, dot);
    if (module.empty()) continue;
    bool found = false;
    for (const std::string& existing : modules) {
      if (existing == module) found = true;
    }
    if (!found) modules.push_back(module);
  }
  return modules;
}

int bundleMain(const std::string& source, const std::string& output,
               const std::string& buildDir, std::string libraryDir) {
  if (!fileExists(source)) {
    std::fprintf(stderr, "cannot find %s\n", source.c_str());
    return 2;
  }
  if (output.empty()) {
    std::fprintf(stderr, "usage: pcc --exe <script.py> <app.exe>\n");
    return 2;
  }

  Program program;
  std::string error;
  if (!compileFile(source, &program, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  const std::vector<std::string> modules = extensionModules(program);

  // 中间字节码放临时目录：它是派生物，不该在源码树里留下 .bc。
  const std::string scratch =
      joinPath(tempDirectory(), "pcc_" + std::to_string(::GetCurrentProcessId()));
  // 创建目录：先试，已存在就用（临时目录同名冲突出错也无所谓，后面会覆盖）。
  ::CreateDirectoryA(scratch.c_str(), nullptr);

  const std::string bytecode = joinPath(scratch, "app.bc");
  if (!writeBytecode(program, bytecode, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  // ── 一律重链接，没有「复制现成 launcher」那条旁路 ──
  //
  // 曾经分两条路：纯脚本直接复制 aipy_app.exe（不用链接器），用到扩展才
  // 重链接。取消它的理由是**第三方库必然要链接**——交付目录里既然一定有
  // 一份能链的语言运行时（core.pylib），就不该再维护第二个基座。
  //
  // 代价：打包一定有 link.exe 的开销（约 1 秒），且机器上必须有链接器。
  // 收益：`dist/` 里只有一个编译器加一个归档目录，没有第二份「同一份入口
  // 代码的另一种形态」，也就没有「哪份在生效」这个问题。
  //
  // 判据不是「脚本有没有用到扩展」而是「有没有地方可链」：零扩展的脚本
  // 同样走这里，只是不点名任何 pylib 符号。
  //
  // dist 里带 lib/core.pylib，拿到它的目录就能重链接，不需要 --build-dir；
  // 否则必须给构建目录（要从里面拿运行时库）。
  //
  // lib-dir 一律转成绝对路径：配方里的 obj 与库都是相对它的，而 link.exe
  // 的工作目录是我们给的临时目录，相对路径在那边会指错地方。
  if (!libraryDir.empty()) libraryDir = absolutePath(libraryDir);
  const bool distMode =
      !libraryDir.empty() &&
      fileExists(joinPath(joinPath(libraryDir, "lib"), "core.pylib"));
  if (buildDir.empty() && !distMode) {
    std::fprintf(stderr,
                 "cannot relink: pass --lib-dir (a directory containing "
                 "lib/core.pylib) or --build-dir\n");
    return 2;
  }
  if (libraryDir.empty()) libraryDir = dirName(buildDir);

  const std::string relinked = joinPath(scratch, "launcher.exe");
  // 产物形态由脚本本身决定：有 `class X(Activity)` 就是图形界面程序。
  //
  // 不给用户加开关的理由：`Activity` 是语言里唯一的类形态，也就是「这个
  // 脚本有界面」的确切信号。多加一个 `--console` / `--gui` 选项等于让同一件
  // 事有两个真相源，而写错的表现是「双击后看不到任何输出」——用户不会知道
  // 该去改哪个开关。
  const Subsystem subsystem =
      program.hasActivity ? Subsystem::Gui : Subsystem::Console;
  LinkResult linkResult;
  if (!relink(buildDir, libraryDir, modules, relinked, program.extensions,
              &linkResult, &error, scratch, subsystem)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  std::uint64_t size = 0;
  if (!bundleExecutable(relinked, bytecode, output, &size, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const std::uint64_t payload = fileSize(bytecode);

  std::printf("%s -> %s (%llu bytes = launcher %llu KB + script %llu bytes)\n",
              source.c_str(), output.c_str(),
              static_cast<unsigned long long>(size),
              static_cast<unsigned long long>((size - payload) / 1024),
              static_cast<unsigned long long>(payload));
  if (!linkResult.functions.empty()) {
    std::printf("  linked functions: ");
    for (std::size_t i = 0; i < linkResult.functions.size(); ++i) {
      std::printf("%s%s", i == 0 ? "" : ", ",
                  linkResult.functions[i].c_str());
    }
    std::printf("\n");
  }
  return 0;
}

// 取出 --flag value 形式的选项。找到就返回 true 并删掉这两项。
bool takeOption(std::vector<std::string>* args, const std::string& flag,
                std::string* value, bool* missing) {
  for (std::size_t i = 0; i < args->size(); ++i) {
    if ((*args)[i] != flag) continue;
    if (i + 1 >= args->size()) {
      *missing = true;
      return false;
    }
    *value = (*args)[i + 1];
    args->erase(args->begin() + static_cast<long>(i),
                args->begin() + static_cast<long>(i) + 2);
    return true;
  }
  return false;
}

}  // namespace

int runCommandLine(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  if (args.empty()) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  pcc <script.py> [out.bc]\n"
                 "  pcc --disasm <file.bc>\n"
                 "  pcc --info <file.bc>\n"
                 "  pcc --exe <script.py> <app.exe> [--build-dir <dir>] "
                 "[--lib-dir <dir>]\n"
                 "  pcc --bundle <launcher.exe> <file.bc> <out.exe>\n"
                 "  pcc --unbundle <app.exe> <out.bc>\n"
                 "  pcc --linked-info <app.exe>\n"
                 "  pcc --pylib-info <module.pylib>\n");
    return 2;
  }

  if (args[0] == "--info") {
    if (args.size() < 2) {
      std::fprintf(stderr, "usage: pcc --info <file.bc>\n");
      return 2;
    }
    LoadedProgram program;
    std::string error;
    if (!loadBytecodeFile(args[1], &program, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    printInfo(program);
    return 0;
  }

  if (args[0] == "--pylib-info") {
    if (args.size() < 2) {
      std::fprintf(stderr, "usage: pcc --pylib-info <module.pylib>\n");
      return 2;
    }
    Pylib pylib;
    std::string error;
    if (!pylib.open(args[1], &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    const PylibManifest& manifest = pylib.manifest();
    std::printf("module: %s\n", manifest.module.c_str());
    std::printf("formatVersion: %d\n", manifest.formatVersion);
    // 多文件包（core.pylib）走 platform_files，platforms 表是空的——
    // 显示成「(none)」会让人以为这个包不支持任何平台。
    const auto files = pylib.filesForHost();
    if (!manifest.platformFiles.empty()) {
      std::printf("platforms: %d (multi-file)\n",
                  static_cast<int>(manifest.platformFiles.size()));
      for (const auto& platform : manifest.platformFiles) {
        std::printf("  %s%s\n", platform.first.c_str(),
                    platform.first == hostPlatformKey() ? "  (host)" : "");
        for (const auto& file : platform.second) {
          std::printf("    %-10s %s\n", file.first.c_str(),
                      file.second.c_str());
        }
      }
      std::printf("host files: %d\n", static_cast<int>(files.size()));
    } else if (manifest.platforms.empty()) {
      std::printf("platforms: (none; nativeLibrary=%s)\n",
                  manifest.nativeLibrary.c_str());
    } else {
      std::printf("platforms: %d\n",
                  static_cast<int>(manifest.platforms.size()));
      for (const auto& entry : manifest.platforms) {
        std::printf("  %-12s %s%s\n", entry.first.c_str(), entry.second.c_str(),
                    entry.first == hostPlatformKey() ? "  (host)" : "");
      }
      const std::string host = pylib.libraryForHost();
      std::printf("host library: %s\n",
                  host.empty() ? "(none available for this platform)"
                               : host.c_str());
    }
    std::printf("bytecode: %s\n",
                manifest.bytecode.empty() ? "(none)" : manifest.bytecode.c_str());
    std::printf("functions: %d\n", static_cast<int>(manifest.functions.size()));
    for (const std::string& function : manifest.functions) {
      std::printf("  %s\n", function.c_str());
    }
    if (!manifest.bareNames.empty()) {
      std::printf("bare names: %d\n",
                  static_cast<int>(manifest.bareNames.size()));
      for (const std::string& name : manifest.bareNames) {
        std::printf("  %s\n", name.c_str());
      }
    }
    return 0;
  }

  if (args[0] == "--linked-info") {
    // 读产物 exe 里实际被链接进来的扩展。给测试用：摘要只是打包器
    // 自己说要点名什么，这里读的是链接器合并出来的结果。
    if (args.size() < 2) {
      std::fprintf(stderr, "usage: pcc --linked-info <app.exe>\n");
      return 2;
    }
    LinkedExtensions linked;
    std::string error;
    if (!readLinkedExtensions(args[1], &linked, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    std::printf("\"modules\": [");
    for (std::size_t i = 0; i < linked.modules.size(); ++i) {
      std::printf("%s\"%s\"", i == 0 ? "" : ", ", linked.modules[i].c_str());
    }
    std::printf("],\n\"functions\": [");
    for (std::size_t i = 0; i < linked.functions.size(); ++i) {
      std::printf("%s[\"%s\", \"%s\"]", i == 0 ? "" : ", ",
                  linked.functions[i].module.c_str(),
                  linked.functions[i].name.c_str());
    }
    std::printf("]\n");
    return 0;
  }

  if (args[0] == "-d" || args[0] == "--disasm") {
    if (args.size() < 2) {
      std::fprintf(stderr, "usage: pcc --disasm <file.bc>\n");
      return 2;
    }
    LoadedProgram program;
    std::string error;
    if (!loadBytecodeFile(args[1], &program, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    disassemble(program, args[1]);
    return 0;
  }

  if (args[0] == "--exe") {
    std::vector<std::string> rest(args.begin() + 1, args.end());
    std::string buildDir;
    std::string libraryDir;
    bool missing = false;
    takeOption(&rest, "--build-dir", &buildDir, &missing);
    if (missing) {
      std::fprintf(stderr, "--build-dir needs a path\n");
      return 2;
    }
    missing = false;
    takeOption(&rest, "--lib-dir", &libraryDir, &missing);
    if (missing) {
      std::fprintf(stderr, "--lib-dir needs a path\n");
      return 2;
    }
    if (rest.size() < 2) {
      std::fprintf(stderr,
                   "usage: pcc --exe <script.py> <app.exe> "
                   "[--build-dir <cmake build dir>] [--lib-dir <pylib dir>]\n");
      return 2;
    }
    return bundleMain(rest[0], rest[1], buildDir, libraryDir);
  }

  if (args[0] == "--bundle") {
    // 只拼装：把现成的 .bc 接到一个**最小链接**出的 launcher 尾部。
    //
    // 与 --exe 的区别是「不编译、不做按需链接」：给的是已经编好的字节码，
    // 产物里一个 pylib 符号都不点名。测试要用它造「字节码用了扩展但产物
    // 没链进来」的情况——那条路径在生产流程里不可达（打包总会链），
    // 但 VM 的「扩展函数没有链接进来」报错必须验。
    std::vector<std::string> rest(args.begin() + 1, args.end());
    std::string buildDir;
    std::string libraryDir;
    bool missing = false;
    takeOption(&rest, "--build-dir", &buildDir, &missing);
    if (missing) {
      std::fprintf(stderr, "--build-dir needs a path\n");
      return 2;
    }
    missing = false;
    takeOption(&rest, "--lib-dir", &libraryDir, &missing);
    if (missing) {
      std::fprintf(stderr, "--lib-dir needs a path\n");
      return 2;
    }
    if (rest.size() < 2) {
      std::fprintf(stderr,
                   "usage: pcc --bundle <file.bc> <out.exe> "
                   "[--build-dir <dir>] [--lib-dir <dir>]\n");
      return 2;
    }

    // lib-dir 与 --exe 同一条规则：给了就用，否则从 build-dir 推。
    if (!libraryDir.empty()) libraryDir = absolutePath(libraryDir);
    const bool distMode =
        !libraryDir.empty() &&
        fileExists(joinPath(joinPath(libraryDir, "lib"), "core.pylib"));
    if (buildDir.empty() && !distMode) {
      std::fprintf(stderr,
                   "cannot relink: pass --lib-dir (a directory containing "
                   "lib/core.pylib) or --build-dir\n");
      return 2;
    }
    if (libraryDir.empty()) libraryDir = dirName(buildDir);

    const std::string scratch =
        joinPath(tempDirectory(), "pcc_" + std::to_string(::GetCurrentProcessId()));
    ::CreateDirectoryA(scratch.c_str(), nullptr);
    const std::string bare = joinPath(scratch, "bare_launcher.exe");

    // 空模块表、空扩展表：产物里只有语言运行时 + launcher 入口。
    LinkResult linkResult;
    std::string error;
    if (!relink(buildDir, libraryDir, {}, bare, {}, &linkResult, &error,
                scratch)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }

    std::uint64_t size = 0;
    if (!bundleExecutable(bare, rest[0], rest[1], &size, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    std::printf("%s -> %s (%llu bytes)\n", rest[0].c_str(), rest[1].c_str(),
                static_cast<unsigned long long>(size));
    return 0;
  }

  if (args[0] == "--unbundle") {
    if (args.size() < 3) {
      std::fprintf(stderr, "usage: pcc --unbundle <app.exe> <out.bc>\n");
      return 2;
    }
    std::uint64_t size = 0;
    std::string error;
    if (!extractBytecode(args[1], args[2], &size, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    std::printf("%s -> %s (%llu bytes)\n", args[1].c_str(), args[2].c_str(),
                static_cast<unsigned long long>(size));
    return 0;
  }

  const std::string source = args[0];
  if (!fileExists(source)) {
    std::fprintf(stderr, "cannot find %s\n", source.c_str());
    return 2;
  }
  const std::string output =
      args.size() > 1 ? args[1] : stripExtension(source) + ".bc";

  Program program;
  std::string error;
  if (!compileFile(source, &program, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  if (!writeBytecode(program, output, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::printf("%s -> %s (%llu bytes, %d functions, %d module instructions)\n",
              source.c_str(), output.c_str(),
              static_cast<unsigned long long>(fileSize(output)),
              static_cast<int>(program.functions.size()),
              static_cast<int>(program.moduleCode.size()));
  return 0;
}

}  // namespace pcc
