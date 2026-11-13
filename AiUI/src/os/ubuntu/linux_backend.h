// Linux 的系统能力实现（尚未实现）
//
// 接法：实现 src/os/os.h 的 Backend，并在本目录提供 backend() 的返回。
// 各方法体照 Windows 那份写：文件用 POSIX 调用，对话框用 GTK 或 xdg-desktop-portal，
// 外壳集成用 .desktop 文件，安装用 XDG 目录约定。
#pragma once

#include "../os.h"

#if defined(__linux__)

namespace AiUI {
namespace os {
namespace linux {

class LinuxBackend final : public Backend {
  // TODO: 与 src/os/windows/windows_backend.h 同形，逐个实现
};

}  // namespace linux
}  // namespace os
}  // namespace AiUI

#endif
