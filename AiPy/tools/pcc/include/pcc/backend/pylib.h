// `.pylib`：一个模块的交付物。
//
// ── 为什么不是一堆 `.lib` ──
//
// 设计理念第二条：第三方能力以 `pylib` 进来。**交付物**应当是一个实体，
// 而不是「一个 .lib + 若干约定」。所以 `.pylib` 是一个 ZIP 归档：
//
//     threading.pylib
//       manifest.json                 模块名、格式版本、提供的函数、平台表
//       lib/win-x64/threading.lib     原生部分（按 .obj 粒度，供函数级按需）
//       bytecode/threading.bc         库自带的 Python 层（可选）
//
// 好处不只是「好看」：库提供哪些函数从「解析 link.exe 的文本输出」变成
// 「读自己的 manifest」，那是现在最脆的一环。
//
// ── 为什么静态库按平台分目录 ──
//
// 一个 `.pylib` 可以同时带 Windows 与 Linux 的原生实现。归档布局本身就能
// 回答「这个库支持哪些平台」——不解包、不读任何额外文件，看目录名即可。
// manifest 的 `platforms` 把这件事写成显式字段，读取端据此挑当前平台的
// 那一个；缺了才退回 `nativeLibrary`（单平台库的简化写法）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pcc/backend/zip.h"

namespace pcc {

// manifest.json 的内容。
struct PylibManifest {
  std::string module;                 // 模块名，如 "threading"
  int formatVersion = 1;
  std::string nativeLibrary;          // 归档内路径；单平台库可以直接写它
  // 平台 → 归档内路径，键形如 "win-x64"、"linux-x64"。
  // 两个字段都在时优先用本表——它才表达得了「一个包带多份实现」。
  std::vector<std::pair<std::string, std::string>> platforms;
  // 平台 → 多个文件（名字 → 归档内路径）。给 `core.pylib` 这类
  // 「一个平台要带好几个库」的包用：运行时是 aipy.lib + aipy_base.lib +
  // launcher.lib 三件套，它们必须一起出现、一起解出来。
  //
  // 为什么与 platforms 分成两个字段而不是统一成「平台 → 列表」：绝大多数
  // 模块就是一个库里一个文件，让它们也写列表形态会让 manifest 变啰嗦，
  // 而且老读取端（只看 platforms）会读不懂。
  std::vector<std::pair<std::string, std::vector<std::pair<std::string,
                                                          std::string>>>>
      platformFiles;
  std::string bytecode;               // 归档内路径，可为空
  std::vector<std::string> functions; // 提供的导出函数名（不含模块前缀）
  // 裸名：脚本里可以直接写的短名。`fs` 的 manifest 里会是 {"open"}，
  // 意思是 `open(...)` 等价于 `fs.open(...)`。
  //
  // 为什么放在 manifest 而不是让链接器去符号表里猜：链接器需要**在打开
  // 归档之前**就知道「哪些库声明了 open」，才能挑出该链的那几个。挨个解包
  // 读符号表既慢，又要把「哪个符号是裸名」这条规则复制一份。
  std::vector<std::string> bareNames;
  // 这个库要求什么样的可执行子系统：空表示不关心（控制台），"gui" 表示
  // 产物必须没有控制台窗口。
  //
  // 为什么由库声明：`tray` 的实现是窗口消息循环，控制台子系统下也能跑，
  // 只是会在背后多一个黑窗口。那是不是错误取决于库在做什么，不该让脚本
  // 作者去记一个 `--gui` 开关。
  std::string subsystem;
};

// 当前构建目标的平台键（"win-x64"）。不认识的平台由调用方报错。
std::string hostPlatformKey();

// 一个已打开的 `.pylib`。
class Pylib {
 public:
  // 从文件打开并读 manifest。失败返回 false。
  bool open(const std::string& path, std::string* error);

  const PylibManifest& manifest() const { return manifest_; }
  const std::string& path() const { return path_; }

  // 当前平台该解哪一个原生库（归档内路径）。没有可用实现时返回空串。
  // 报错细节（比如「包里只有 linux-x64，本机是 win-x64」）另由
  // extractLibrary 给——这里只负责挑。
  std::string libraryForHost() const;

  // 把原生静态库解出来写到 outputPath。返回写出的字节数。
  //
  // 为什么解到磁盘而不是内存：`link.exe` 只接受文件路径。所以打包一个
  // 用到扩展的脚本时，流程是「解 .lib 到临时目录 → 链接 → 删临时目录」。
  bool extractLibrary(const std::string& outputPath, std::uint64_t* size,
                      std::string* error) const;

  // 这个包在本平台带了哪些文件：角色 → 归档内路径。
  //
  // 单文件库（绝大多数模块）用 platforms 表；多文件包（core.pylib）用
  // platform_files 表。两者都返回同一种形状，调用方不必分情况。
  //
  // 角色名对单文件库就是模块名——没有更好的名字可用，而调用方反正只取
  // 那一个。
  std::vector<std::pair<std::string, std::string>> filesForHost() const;

  // 按角色解出文件。角色不存在时返回 false 并说明有哪些角色。
  bool extractFile(const std::string& role, const std::string& outputPath,
                   std::uint64_t* size, std::string* error) const;

  // 取出库自带的字节码（没有就返回 false）。
  bool readBytecode(std::vector<std::uint8_t>* out, std::string* error) const;

 private:
  std::string path_;
  ZipArchive archive_;
  PylibManifest manifest_;
};

// 在目录里找 `<module>.pylib`（也接受 `lib/` 子目录）。
// 找不到返回空串。
std::string findPylibArchive(const std::string& directory,
                             const std::string& module);

// 解析 manifest.json 文本。失败返回 false。
bool parseManifest(const std::string& text, PylibManifest* manifest,
                   std::string* error);

}  // namespace pcc
