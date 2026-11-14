// AiInstaller 的 Windows 后端
//
// 需要平台判断的只剩「哪些盘可用、哪个是系统盘」，其余动作（落盘、快捷方式、
// 卸载入口、提权）都由 AiUI 的中性接口完成，这里只做转发与少量平台语义填补。
#include "AiInstaller/path_policy.h"

#include "AiUI/platform.h"

namespace AiInstaller {

// 目前没有需要 Windows 特有实现的部分：卷枚举与系统盘判定已由
// AiUI::DriveInfo::system 提供。保留本文件是为了给后续平台差异留位置
// （例如卷可用空间查询、就地升级时的文件占用检测）。
//
// 不实现空翻译单元警告：保留一个内部链接的符号。
namespace {
constexpr int kWindowsBackendMarker = 1;
}  // namespace

int windowsBackendMarker() { return kWindowsBackendMarker; }

}  // namespace AiInstaller
