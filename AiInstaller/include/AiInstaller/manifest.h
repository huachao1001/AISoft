// 安装包清单：一个文本文件说清「装什么、界面显示什么、文件在哪」
//
// 为什么用 INI 而不是 JSON/XML：安装包清单要被构建脚本手写、被排查的人
// 直接打开看，键值文本最省事，也不需要在安装器里塞一个解析器依赖。
//
// 清单同时承担三件事：
//   1. 身份：显示名、版本、主程序名、安装子目录（默认路径据此推算）
//   2. 界面：窗口标题、标题、说明、按钮文字、图标（布局结构固定）
//   3. 载荷：包内每个文件的位置与大小（由打包器写入）
//
// 因此安装器不需要为每个应用编一份代码——同一个 exe，换清单就是换应用。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AiInstaller {

// 包内一个文件。offset 相对载荷区起点，由打包器填写。
struct ManifestFile {
  // 安装后的相对路径，如 "AiZip.exe"、"config\x.dll"
  std::wstring path;
  // 相对载荷区起点的字节偏移
  uint64_t offset = 0;
  // 字节数
  uint64_t size = 0;
  // false 表示只随包携带、不装到磁盘。
  // 界面图标这类品牌素材属于这一类：安装器要读它，用户目录里不需要它。
  bool install = true;
};

struct Manifest {
  // ── 身份（[app]）──
  std::wstring displayName;
  std::wstring version;
  std::wstring publisher;
  // 主程序文件名，快捷方式与「装完启动」都指向它
  std::wstring exeName;
  // 安装子目录，不含盘符，如 "AISoft\AiZip"
  std::wstring installSubdir;
  // 安装完成后要执行的命令，可带参数，如 "AiZip.exe" 或 "AiZip.exe --open"。
  // 留空表示装完什么都不启动。
  //
  // 不写成「启动主程序」的布尔值：有的应用装完要跑一次初始化工具，
  // 有的要带参数打开，给一条命令同时覆盖这些情形，清单也不必为此加字段。
  std::wstring afterInstall;

  // ── 界面（[ui]）──
  // 以下留空时由界面层给出合理默认，清单可以只写最少的几项
  std::wstring windowTitle;
  std::wstring title;
  std::wstring description;
  // 包内 logo 图片（png/ico 等），显示在标题左侧；可空
  std::wstring logo;
  std::wstring installText = L"安装";
  std::wstring cancelText = L"取消";
  std::wstring browseText = L"浏览...";
  std::wstring allUsersText = L"所有用户";
  std::wstring currentUserText = L"仅当前用户";
  // 是否显示「安装范围」一行。只支持当前用户的应用可以关掉它，
  // 免得用户选了一个注定要提权失败的选项。
  bool showScope = true;

  // ── 配置模块（[config]）──
  // 安装目录里的配置 dll 文件名，按 AiInstaller/config_abi.h 的标准 ABI 导出。
  // 留空表示该应用没有任何系统配置，安装器装完文件就结束。
  std::wstring configModule;

  // ── 载荷（[files]）──
  // 源清单里只写 path；打包器补上 offset/size。
  std::vector<ManifestFile> files;
};

// 解析清单文本。成功返回 true；失败时 error 里是行号与原因。
bool parseManifest(const std::wstring& text, Manifest& out, std::wstring& error);

// 序列化成清单文本（UTF-8 编码由调用方负责）。
std::wstring formatManifest(const Manifest& manifest);

}  // namespace AiInstaller
