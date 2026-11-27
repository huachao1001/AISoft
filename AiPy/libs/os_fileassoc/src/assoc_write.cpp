// 写关联：扩展名键、ProgID 的 open 命令、图标。
//
// ── 三层结构 ──
//
//   .mydata                          默认值 = ProgID
//   <ProgID>\shell\open\command      默认值 = "exe" "%1"
//   <ProgID>\DefaultIcon             默认值 = "exe",0
//
// 第一层是「这个扩展名归哪个 ProgID」，后两层是「那个 ProgID 怎么启动、
// 显示什么图标」。分开存是 Windows 的模型：多个扩展名可以共用一个 ProgID
// （`.jpg` 与 `.jpeg` 都指向 `jpegfile`），改启动命令只要改一处。
//
// ── UserChoice 写不了 ──
//
// Win8 之后，用户在「打开方式」里选过的扩展名会多一条
// `HKCU\...\FileExts\<ext>\UserChoice`，它的值带一个用扩展名与 SID 算出的
// 哈希，系统会校验。程序**无法**合法写入它（微软刻意这么设计，防程序互相
// 抢关联）。
//
// 所以本库能做的是「登记一个候选」：全新扩展名直接生效；已经被别的程序
// 占用的扩展名，系统会弹「你要用什么打开」，用户选一次我们的程序就好。
// 硬改 UserChoice 的后果是关联显示成「未知应用」——那比不做更糟。
#include "aipy/os_fileassoc/fileassoc.h"
#include "assoc_keys.h"

#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

namespace aipy {
namespace os_fileassoc {
namespace {

// 默认 ProgID：`.mydata` → `AiPyFile.mydata`。
//
// 带 `AiPyFile.` 前缀是为了不撞上常见 ProgID（`txtfile`、`jpegfile`）。
std::string defaultProgId(const std::string& extension) {
  std::string name = "AiPyFile";
  if (extension.size() > 1) name += extension;  // extension 自带前导点
  return name;
}

// 把路径包进引号。`%1` 是系统替换的占位符，也必须有引号，否则路径里的
// 空格会把参数切断——症状是「双击一个放在 "My Documents" 里的文件，
// 程序收到的是 C:\Users\...\My」。
std::string quote(const std::string& text) { return "\"" + text + "\""; }

}  // namespace

std::string registerExtension(const std::string& extension,
                              const std::string& progId) {
  const std::string ext = normalizeExtension(extension);
  if (ext.empty()) return "invalid extension: '" + extension + "'";
  const std::string exe = executablePath();
  if (exe.empty()) return "cannot determine this executable's path";

  const std::string id = progId.empty() ? defaultProgId(ext) : progId;

  // ── 1. 扩展名 → ProgID ──
  std::string error = writeString(ext, std::string(), id);
  if (!error.empty()) return error;

  // ── 2. 启动命令 ──
  error = writeString(id + "\\shell\\open\\command", std::string(),
                      quote(exe) + " \"%1\"");
  if (!error.empty()) return error;

  // ── 3. 图标（exe 的第 0 个图标资源）──
  // 已经有自定义图标的（用户调过 icon()）不覆盖：重装关联不该把图标改回去。
  const std::string iconKey = id + "\\DefaultIcon";
  if (readString(iconKey, std::string()).empty()) {
    error = writeString(iconKey, std::string(), quote(exe) + ",0");
    if (!error.empty()) return error;
  }

  // 让资源管理器立刻刷新关联缓存。不调的话「双击没反应」会持续到下次登录，
  // 用户会以为没装成功。
#if defined(_WIN32)
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
  return std::string();
}

std::string unregisterExtension(const std::string& extension) {
  const std::string ext = normalizeExtension(extension);
  if (ext.empty()) return "invalid extension: '" + extension + "'";

  // ── 只删自己的 ──
  //
  // 扩展名现在指向别的 ProgID 时不动它：那可能是用户在「打开方式」里选的，
  // 也可能是另一个程序装的。卸载自己的应用不该顺手拆掉别人的关联。
  const std::string current = readString(ext, std::string());
  const std::string ours = defaultProgId(ext);
  const bool ownedByUs = current == ours ||
                         (current.size() > 4 &&
                          current.compare(0, 4, "AiPy") == 0);
  if (!ownedByUs) return std::string();

  std::string error = deleteKey(ext);
  if (!error.empty()) return error;
  // 只删我们自己那份 ProgID，`ours` 与 `current` 不同时说明是自定义名，
  // 由调用方自己管理。
  if (current == ours) {
    error = deleteKey(ours);
    if (!error.empty()) return error;
  }

#if defined(_WIN32)
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
  return std::string();
}

std::string setIcon(const std::string& extension, const std::string& iconPath) {
  const std::string ext = normalizeExtension(extension);
  if (ext.empty()) return "invalid extension: '" + extension + "'";

  const std::string progId = readString(ext, std::string());
  if (progId.empty()) return "extension " + ext + " is not registered";

  // 空路径 → 退回 exe 自带的图标。
  const std::string target =
      iconPath.empty() ? quote(executablePath()) + ",0" : quote(iconPath);
  const std::string error =
      writeString(progId + "\\DefaultIcon", std::string(), target);
  if (!error.empty()) return error;

#if defined(_WIN32)
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
  return std::string();
}

}  // namespace os_fileassoc
}  // namespace aipy
