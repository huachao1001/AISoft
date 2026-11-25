// 把内嵌脚本跑起来——「打包成 exe」的执行端。
//
// ── 两种用法 ──
//
//   1. 裸 launcher：`aipy_app.exe some.bc`，跑指定的字节码
//   2. 打包后的应用：`app.exe`，跑**自己文件尾部**的字节码
//
// 第二种才是交付形态。打包不需要 C++ 编译器：`aipyic.py --exe` 把字节码
// 追加到这个 exe 的尾部再写一个索引，产物就能直接双击运行。用户机器上
// 既不需要 MSVC，也不需要 Python——需要的只有这个 exe 本身。
//
// ── 为什么先看参数，再退回内嵌 ──
//
// 保留「裸 launcher 带文件参数」这一条，是因为开发和排错时不想每次都
// 重新打包一遍（打一次要拷 400 KB）。正式产物不带参数，走内嵌路径。
#include "aipy/bytecode.h"
#include "aipy/vm.h"

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// UTF-8 → UTF-16。`std::wstring(utf8.begin(), utf8.end())` 是**错的**：
// 它把每个字节当成一个 wchar_t，一个汉字（3 字节）会被拆成 3 个无意义
// 字符，弹窗里显示成日文一样的乱码。这个坑踩过。
std::wstring toWide(const std::string& utf8) {
#if defined(_WIN32)
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()),
                                         nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                      static_cast<int>(utf8.size()), &wide[0], needed);
  return wide;
#else
  (void)utf8;
  return std::wstring();
#endif
}

// 控制台归属：决定「这个进程是不是被双击起来的」。
//
// 实测三种情况（GetConsoleProcessList 的返回值）：
//
//   0  没有控制台          → GUI 子系统产物（双击图形程序）
//   1  独占一个控制台      → 双击控制台程序，控制台是系统为它新建的
//   2+ 与别的进程共用      → 从 cmd / PowerShell 里调用
//
// 这是唯一可靠的判据。`_isatty` 不行：双击时 stdout 也是 tty；看 argv[0]
// 更不行：从快捷方式启动时它可能是别的东西。
enum class ConsoleOwnership { None, Exclusive, Shared };

ConsoleOwnership consoleOwnership() {
#if defined(_WIN32)
  DWORD pids[2];
  const DWORD count = GetConsoleProcessList(pids, 2);
  if (count == 0) return ConsoleOwnership::None;
  if (count == 1) return ConsoleOwnership::Exclusive;
  return ConsoleOwnership::Shared;
#else
  return ConsoleOwnership::Shared;
#endif
}

// Windows 上不带控制台的 GUI 子系统产物没有 stderr 可写；统一走
// OutputDebugString，命令行下 attach 到父控制台才看得见。
//
// 完全没有控制台时（双击 GUI 程序）再弹一个消息框：否则脚本报错后窗口
// 什么都不显示，用户只知道「点了没反应」。
void report(const std::string& text) {
#if defined(_WIN32)
  const std::wstring wide = toWide(text);
  if (!wide.empty()) {
    OutputDebugStringW((wide + L"\n").c_str());
    if (consoleOwnership() == ConsoleOwnership::None) {
      MessageBoxW(nullptr, wide.c_str(), L"AiPy", MB_OK | MB_ICONERROR);
    }
  }
#endif
  std::fprintf(stderr, "%s\n", text.c_str());
  std::fflush(stderr);
}

// 用 C 文件 API 而不是 `std::ifstream` + `istreambuf_iterator`。
//
// 这不是风格偏好：那套 iostream 代码会把 `basic_ifstream`、`codecvt`、
// `use_facet` 与整套格式化机器一起拖进产物，实测多出 66 KB（.text 里
// 一个叫 `i` 的模块）。launcher 只需要「读一个文件到内存」，用不到
// locale、用不到格式化。
std::vector<std::uint8_t> readWhole(const char* path) {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) return {};

  std::vector<std::uint8_t> data;
  std::uint8_t chunk[16 * 1024];
  for (;;) {
    const std::size_t got = std::fread(chunk, 1, sizeof(chunk), file);
    if (got == 0) break;
    data.insert(data.end(), chunk, chunk + got);
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) return {};
  return data;
}

// 读自己。不能靠 argv[0]：Windows 上它可能是相对路径、可能没有扩展名，
// 而且从快捷方式启动时还可能是别的东西。`GetModuleFileNameW` 给的是
// 内核记录的真实映像路径，这是唯一可靠来源。
std::vector<std::uint8_t> readSelf(const char* argv0) {
#if defined(_WIN32)
  // Windows 上 argv[0] 可能是相对路径、可能没有扩展名、从快捷方式启动时
  // 还可能是别的东西。GetModuleFileNameW 给的是内核记录的真实映像路径，
  // 这是唯一可靠来源，所以这里刻意不用参数（见上面的定义）。
  (void)argv0;
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    // 返回值等于缓冲区大小说明路径被截断了，需要更大的缓冲区。
    if (static_cast<std::size_t>(length) < buffer.size()) {
      // 必须走 UTF-8 转换：直接按宽字节迭代器构造窄串在中文用户名、
      // 中文目录下会得到乱码路径，读文件直接失败。
      const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer.data(),
                                             static_cast<int>(length), nullptr, 0,
                                             nullptr, nullptr);
      if (needed <= 0) return {};
      std::string narrow(static_cast<std::size_t>(needed), '\0');
      WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(length),
                          &narrow[0], needed, nullptr, nullptr);
      return readWhole(narrow.c_str());
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  return readWhole(argv0);
#endif
}

int runProgram(const aipy::vm::Program& program, const char* name) {
  aipy::vm::Machine machine;
  const aipy::vm::Result result = machine.run(program);
  if (result.failed) {
    if (result.line > 0) {
      report(std::string(name) + ":" + std::to_string(result.line) + ": " + result.error);
    } else {
      report(std::string(name) + ": " + result.error);
    }
    return 1;
  }
  return 0;
}

// 双击启动的控制台程序，跑完窗口会立刻消失——用户只看到一闪，什么也读不到。
//
// 只在**独占**控制台时等：从 cmd / PowerShell 里调用是共用控制台，等一次
// 回车会把批处理和脚本卡住，那是最讨厌的那类副作用。
void holdIfLaunchedByDoubleClick() {
  if (consoleOwnership() != ConsoleOwnership::Exclusive) return;
  std::fputs("\nPress Enter to exit...", stdout);
  std::fflush(stdout);
  // 读到 EOF 就直接返回：某些环境下 stdin 已经关了，不该在这死等。
  while (std::fgetc(stdin) != '\n' && !std::feof(stdin)) {
  }
}

// 让控制台按 UTF-8 解释输出。
//
// 双击时那个控制台是系统新建的，代码页是系统默认（中文 Windows 是 936），
// 而脚本的字符串在内存里是 UTF-8——不切代码页，中文输出全是乱码。
//
// 只改输出代码页，不改输入：输入代码页影响 `std::fgetc` 的字节含义，
// 改了会让「按回车」这类交互在非 UTF-8 键盘布局下行为不确定。
void useUtf8Console() {
#if defined(_WIN32)
  SetConsoleOutputCP(CP_UTF8);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  useUtf8Console();

  // 脚本的 `print` 走 log sink。语言层默认写 stderr 并加 `[aipy]` 前缀
  // （调试诊断的口径）；应用产物要和 `python script.py` 一样只看程序输出，
  // 所以这里换成写 stdout、不加前缀。
  //
  // 必须 flush：一旦脚本用了 threading.start，进程退出时后台线程还在跑，
  // stdout 的缓冲有可能来不及丢出去——症状是「程序有输出但重定向到文件
  // 是空的」。`python script.py` 是行缓冲，这里对齐它。
  aipy::setLogSink([](const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  });

  // 给了参数就按「裸 launcher」跑指定文件，方便开发期不重打包。
  if (argc >= 2) {
    const std::vector<std::uint8_t> data = readWhole(argv[1]);
    if (data.empty()) {
      report(std::string("读不到 ") + argv[1]);
      return 2;
    }
    const aipy::bytecode::Result loaded = aipy::bytecode::load(data);
    if (loaded.failed) {
      report(std::string(argv[1]) + ": " + loaded.error);
      return 1;
    }
    return runProgram(loaded.program, argv[1]);
  }

  // 没参数：这是打包产物，跑自己尾部的字节码。
  const std::vector<std::uint8_t> self = readSelf(argv[0]);
  if (self.empty()) {
    report("读不到自己的映像文件");
    return 2;
  }
  const aipy::bytecode::Result loaded = aipy::bytecode::loadAttached(self.data(), self.size());
  if (loaded.failed) {
    report(loaded.error);
    report("用法: aipy_app <script.bc>（或把它打包成 exe 后直接运行）");
    holdIfLaunchedByDoubleClick();
    return 2;
  }
  const int status = runProgram(loaded.program, "app");
  // 出错时也等：双击的程序报错后窗口立刻消失，等于什么也没说。
  holdIfLaunchedByDoubleClick();
  return status;
}
