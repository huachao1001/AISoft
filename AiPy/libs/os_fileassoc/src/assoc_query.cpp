// 查询：扩展名现在关联到哪个 ProgID、是不是本程序。
//
// 只读，不写注册表。单独一个编译单元：脚本只想知道「装没装上」时不该把
// 写关联与图标设置的代码拖进产物。
#include "aipy/os_fileassoc/fileassoc.h"
#include "assoc_keys.h"

#include <string>

namespace aipy {
namespace os_fileassoc {

std::string queryExtension(const std::string& extension) {
  const std::string ext = normalizeExtension(extension);
  if (ext.empty()) return std::string();
  return readString(ext, std::string());
}

bool isOwnedByThisExe(const std::string& extension) {
  const std::string ext = normalizeExtension(extension);
  if (ext.empty()) return false;

  const std::string progId = readString(ext, std::string());
  if (progId.empty()) return false;

  // 判据是**启动命令里的 exe 路径**，不是 ProgID 名字：用户可能自己起了
  // 一个别名 ProgID，但只要它指向本 exe，关联就是我们的。
  const std::string command =
      readString(progId + "\\shell\\open\\command", std::string());
  if (command.empty()) return false;
  const std::string exe = executablePath();
  if (exe.empty()) return false;
  return command.find(exe) != std::string::npos;
}

}  // namespace os_fileassoc
}  // namespace aipy
