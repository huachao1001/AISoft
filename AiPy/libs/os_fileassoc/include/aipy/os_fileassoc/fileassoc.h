// `os.fileassoc` pylib 的对外契约。
//
// ── 它做什么 ──
//
// 把一个扩展名关联到当前 exe，双击就运行它：
//
//   import os.fileassoc
//   os.fileassoc.register(".mydata", "MyData.File")   # 写入关联
//   os.fileassoc.icon(".mydata", "app.ico")           # 显示图标
//
// 之后双击 `x.mydata` 就启动本程序，脚本靠 `sys.argv()` 拿到那个路径。
//
// ── 为什么只写 HKCU ──
//
// `HKEY_CLASSES_ROOT` 是 `HKLM\Software\Classes` 与
// `HKCU\Software\Classes` 的合并视图。往 HKLM 写需要管理员权限，而「装个
// 应用」不该弹 UAC——那会把用户吓退，也会在企业环境里被策略拦掉。
//
// 只写 HKCU 的代价：关联只对当前用户生效，别的用户登录后要各自装一次。
// 这是 Windows 推荐的做法（Per-User Registration），也正是我们想要的——
// 「谁装谁用」比「装一次影响全机器」更符合一个便携 exe 的定位。
//
// ── 关联的三层结构 ──
//
//   HKCU\Software\Classes\.mydata                   默认值 = ProgID
//   HKCU\Software\Classes\MyData.File\shell\open\command  = "路径" "%1"
//   HKCU\Software\Classes\MyData.File\DefaultIcon      = "路径",0
//
// `%1` 会被系统替换成被双击的文件路径（带引号传入，因为路径可能有空格）。
// 图标取 exe 的第 0 个图标资源；脚本想用单独的 .ico 就调 `icon()`。
#pragma once

#include <string>

namespace aipy {
namespace os_fileassoc {

// 当前 exe 的绝对路径。写进 command 的就是它。
//
// 用 `GetModuleFileNameW` 而不是 argv[0]：后者可能是相对路径、可能没有
// 扩展名，从快捷方式启动时还可能是别的东西。
std::string executablePath();

// 把一个扩展名关联到当前 exe。`progId` 空时按扩展名生成（`.mydata` →
// `AiPy.mydata`）。返回失败原因，成功时为空串。
//
// 写三处：扩展名键的默认值、ProgID 的 open 命令、ProgID 的图标。
std::string registerExtension(const std::string& extension,
                              const std::string& progId);

// 撤销关联。只删自己写的那几个键；扩展名被别的 ProgID 占用时不动它。
std::string unregisterExtension(const std::string& extension);

// 给关联设置自定义图标文件。`iconPath` 空表示退回 exe 自带的图标。
std::string setIcon(const std::string& extension, const std::string& iconPath);

// 查询扩展名当前关联到哪个 ProgID（空表示没有关联）。
std::string queryExtension(const std::string& extension);

// 这个扩展名是不是已经关联到当前 exe。
bool isOwnedByThisExe(const std::string& extension);

// 扩展名归一：去掉首尾空白、补上前导点、转小写。非法时返回空串。
//
// 单独一个函数是因为四个导出函数都要用，而「`.TXT` 与 `txt` 是不是同一个」
// 这种判断必须只有一处实现——分开写必然有一处漏了大小写。
std::string normalizeExtension(const std::string& extension);

}  // namespace os_fileassoc
}  // namespace aipy
