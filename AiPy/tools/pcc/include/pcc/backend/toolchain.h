// 平台分派：把「哪些是平台细节」收在一处。
//
// ── 只有这几件事与操作系统绑定 ──
//
//   1. 用哪个链接器、参数长什么样（MSVC: `/INCLUDE:`；GNU: `-u`）
//   2. 原生静态库的扩展名（`.lib` / `.a`）
//   3. 默认带的系统库
//   4. 平台键（`win-x64` / `linux-x64`）——`.pylib` 的 manifest 用它挑实现
//
// 抽出来的目的是将来加一个平台只改本文件，不动 link 的按需逻辑。
// 当前只实现 Windows/MSVC——不是因为懒，而是唯一有真实验证路径的那个。
// 别的平台留接口而不是半成品代码：半成品会被误以为能用。
#pragma once

#include <string>
#include <vector>

namespace pcc {

// 产物形态：控制台程序，还是图形界面程序。
//
// 两者是**同一个 launcher**，只差链接器的一个参数——不需要第二份入口代码。
// 选错的表现很具体：控制台程序双击时窗口一闪而过（能看到输出但来不及看），
// 图形界面程序双击时完全不出现控制台（脚本的 print 无处可去）。
enum class Subsystem {
  Console,
  Gui,
};

class Toolchain {
 public:
  virtual ~Toolchain() = default;

  // 原生静态库的扩展名，带点：Windows 是 ".lib"，Linux/macOS 是 ".a"。
  //
  // 为什么这是平台细节而不是「`.pylib` 里固定写死的」：`.pylib` 是一个
  // **跨平台**归档，同一个包里 Windows 那份叫 `<模块>.lib`、Linux 那份叫
  // `<模块>.a`。归档内路径由 manifest 的 platforms 表给出，但解出来落到
  // scratch 时要用本平台的扩展名——链接器按扩展名判断它是不是库。
  virtual std::string librarySuffix() const = 0;
  virtual std::string symbolFor(const std::string& module,
                                const std::string& function) const = 0;
  virtual std::string moduleSymbol(const std::string& module) const = 0;
  // 裸名注册项的符号：`aipy_<前缀>_bare_<短名>`。
  // 前缀是模块名把点换成下划线（`os.path` → `os_path`），与 C++ 侧
  // AIPY_PYLIB_BUILTIN 展开出的符号一致。
  virtual std::string bareSymbol(const std::string& module,
                                 const std::string& bare) const = 0;
  virtual std::string quoteInclude(const std::string& symbol) const = 0;
  virtual std::string outputFlag(const std::string& path) const = 0;
  // 产物子系统的链接参数。
  //
  // GUI 必须额外点名 `mainCRTStartup`：`/subsystem` 只改 PE 头里的子系统
  // 字段，不改「CRT 取哪个入口」，而链接器会按子系统默认挑入口函数——
  // GUI 下它去找 `WinMain`，而 launcher 只有 `main`。实测过这条组合：
  // 同一个 `main` 加上这两个参数就是合法 GUI 产物，不需要第二份入口代码。
  virtual std::vector<std::string> subsystemFlags(Subsystem kind) const = 0;
  // 默认链接的系统导入库（kernel32 等）。
  //
  // 它是平台细节：Windows 要 kernel32/user32/..., Linux 是 libc/libm/libpthread。
  // 放在 link.cpp 里就等于把「哪个平台」这条信息复制了一份到按需链接逻辑里。
  virtual std::vector<std::string> systemLibraries() const = 0;
  // 构建目录里 launcher 入口库的文件名（`aipy_launcher.lib` / `aipy_launcher.a`）。
  //
  // 构建模式用它当入口：库里的 `main` 没人引用，所以链接时得点名。
  // dist 模式不用它——那边从 core.pylib 按角色 `launcher` 取。
  virtual std::string launcherLibraryName() const = 0;
  // 库里有哪些外部符号。读不出来时返回空集。
  //
  // 只给测试与诊断用：正式的按需链接**只信 manifest**，不再从库里反推
  // 提供了哪些函数（那需要解析 COFF/ELF，且失败会静默退化）。
  virtual std::vector<std::string> libraryNames(
      const std::string& path) const = 0;
};

// Windows / MSVC。
//
// 为什么断言 MSVC 而不是「Windows 通用」：符号命名（`aipy_<模块>_fn_<函数>`）
// 与节合并（`.aipyfn$k`）都是本项目 C++ 侧的实现决定的，而那一侧目前只有
// MSVC 分支（见 include/aipy/pylib.h）。
class MsvcToolchain : public Toolchain {
 public:
  MsvcToolchain(std::string linkExe,
                std::vector<std::string> libraryPaths);

  std::string librarySuffix() const override { return ".lib"; }
  std::string symbolFor(const std::string& module,
                        const std::string& function) const override;
  std::string moduleSymbol(const std::string& module) const override;
  std::string bareSymbol(const std::string& module,
                         const std::string& bare) const override;
  std::string quoteInclude(const std::string& symbol) const override;
  std::string outputFlag(const std::string& path) const override;
  std::vector<std::string> subsystemFlags(Subsystem kind) const override;
  std::vector<std::string> systemLibraries() const override;
  std::string launcherLibraryName() const override { return "aipy_launcher.lib"; }
  std::vector<std::string> libraryNames(
      const std::string& path) const override;

  const std::string& linkExe() const { return linkExe_; }

 private:
  std::string linkExe_;
  std::vector<std::string> libraryPaths_;
};

// 按当前平台给出 Toolchain。失败返回 nullptr，原因在 error 里。
//
// 判据是编译期的 `_WIN32`，而不是「链接器路径里有没有 .exe」——后者在交叉
// 编译时会把宿主编译器误判成本地平台。
//
// linkExe 为空时由本函数自己找（见 findLinker），找到的路径填进 linkExe。
Toolchain* makeToolchain(std::string* linkExe,
                         const std::vector<std::string>& libraryPaths,
                         std::string* error);

// 在本机找链接器。找到返回 true 并写出绝对路径。
//
// 为什么不由发布件记下路径：`dist/` 是要被拷到别人机器上的，而链接器的
// 位置是**那台机器**的事实，不是构建机的事实。把构建机的绝对路径写进
// 配方，等于让「装在 D:\vs2022 的机器打出来的 dist」在别人机器上直接失败，
// 而对方明明装着链接器、只是路径不同（实测过这种报错「install the MSVC
// linker」，而链接器就在他的机器上）。
//
// 查找顺序：
//   1. PATH 里的 link.exe（没跑 vcvars 时通常找不到，但有人手工配了）
//   2. vswhere 查已装的 VS / BuildTools（这是官方推荐的做法）
std::string findLinker(std::string* error);

// 从 link.exe 的位置推断 MSVC 与 Windows SDK 的库目录。
//
// SDK 路径无法从 link.exe 推出来（独立安装），所以扫常见根目录。MSVC 库
// 目录则逐级上溯找 `lib/x64/libcpmt.lib`——数 `..` 的层数不可靠，不同版本
// 目录深度会变，数错一级就静默丢掉整个 MSVC 库路径。
std::vector<std::string> libraryPathsFor(const std::string& linkExe);

}  // namespace pcc
