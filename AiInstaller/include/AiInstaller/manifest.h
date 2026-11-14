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

// 清单里登记的一条安装后命令。
//
// 为什么用「命令行」而不是只留配置 dll：有些注册动作本来就是一条命令行
// （reg add、regsvr32、sc create），为一个命令行去编一个 dll 不值当。命令
// 与配置 dll 并存：能一条命令做完的写命令，需要 COM/复杂逻辑的才写 dll。
struct ManifestCommand {
  // 可执行文件。含路径分隔符时直接用；裸文件名（如 regsvr32.exe）由系统
  // 按 PATH 搜索。相对路径（如 AiZip.exe）相对安装目录解析。
  std::wstring path;
  // 参数，原样传给程序
  std::wstring args;
  // 进度条下方显示的文案，如「正在注册右键菜单」。留空时用 path 兜底。
  std::wstring text;
  // true（默认）表示等它执行完再继续；false 表示拉起来就走，不等结果。
  // 静默的注册命令都该是 true——不等就报不出它失败。
  bool wait = true;
  // 失败是否影响安装结果。默认 true：注册命令失败通常意味着装得不完整。
  bool required = true;
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
  // 用户勾选「立即打开」时要执行的命令，可带参数，如 "AiZip.exe" 或
  // "AiZip.exe --open"。留空表示完成后没有可打开的程序（复选框会隐藏）。
  //
  // 不再「装完自动启动」：安装结束时突然弹出一个窗口会打断用户手上的事。
  // 现在这条命令只由完成页的复选框触发，用户不勾就不跑。
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
  // 「立即打开」复选框的文字，与它的默认选中状态。
  // 装完弹出一个程序会打断用户手上的事，所以文案与默认值做成可配置的：
  // 应用自己知道装完是否该顺手打开。
  std::wstring runAfterText = L"立即打开";
  bool runAfterChecked = true;

  // ── 配置模块（[config]）──
  // 安装目录里的配置 dll 文件名，按 AiInstaller/config_abi.h 的标准 ABI 导出。
  // 留空表示该应用没有任何系统配置，安装器装完文件就结束。
  std::wstring configModule;

  // ── 安装后命令（[commands]）──
  // 按清单顺序执行。与配置 dll 并存：dll 负责需要 COM 的复杂注册，
  // 命令负责「一条命令行就能做完」的事。每条的文案会显示在进度条下方。
  std::vector<ManifestCommand> commands;

  // ── 载荷（[files]）──
  // 源清单里只写 path；打包器补上 offset/size。
  std::vector<ManifestFile> files;
};

// 解析清单文本。成功返回 true；失败时 error 里是行号与原因。
bool parseManifest(const std::wstring& text, Manifest& out, std::wstring& error);

// 序列化成清单文本（UTF-8 编码由调用方负责）。
std::wstring formatManifest(const Manifest& manifest);

}  // namespace AiInstaller
