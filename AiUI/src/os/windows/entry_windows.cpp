// Windows 图形入口
//
// 只做「把控制权交给应用」，不含业务逻辑。CRT 的 wWinMainCRTStartup 会引用
// wWinMain，链接器因此会把本目标文件从 AiUI 静态库里拉进来，应用不必自己
// 写平台入口。
#include <windows.h>

#include "AiUI/app.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) { return AiUI::runApp(); }
