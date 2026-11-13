// macOS 的系统能力实现（尚未实现）
//
// 接法：实现 src/os/os.h 的 Backend，并在本目录提供 backend() 的返回。
// 各方法体照 Windows 那份写：文件用 NSFileManager，对话框用 NSOpenPanel /
// NSAlert，外壳集成用 LaunchServices，安装用 .app 布局。
#pragma once

#include "../os.h"

#if defined(__APPLE__)

namespace AiUI {
namespace os {
namespace macos {

class MacBackend final : public Backend {
  // TODO: 与 src/os/windows/windows_backend.h 同形，逐个实现
};

}  // namespace macos
}  // namespace os
}  // namespace AiUI

#endif
