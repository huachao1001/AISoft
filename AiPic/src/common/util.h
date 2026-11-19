// 通用工具：路径、格式化、设置
//
// 不出现平台类型：路径处理全部经 AiUI::Platform，设置写在用户配置目录里。
#pragma once

#include <cstdint>
#include <string>

namespace util {

// ── 路径 ──
std::wstring FileExtension(const std::wstring& path);  // 含点，小写
std::wstring FileStem(const std::wstring& path);        // 去目录去扩展
std::wstring BaseName(const std::wstring& path);        // 去目录
std::wstring DirName(const std::wstring& path);
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring AbsolutePath(const std::wstring& path);
bool PathExists(const std::wstring& path);

// 这些后缀由图像解码器支持（GDI+ 覆盖的集合）。
// 判断后缀而不是去试解码：列一个目录时不能对每个文件都解一遍。
bool IsImageFile(const std::wstring& path);

// 扩展名 → 另存为格式名（"PNG" 这类），只用于状态栏与对话框文案
std::wstring FormatLabel(const std::wstring& path);

// ── 格式化 ──
// 缩放比例转百分比文案："100%"
std::wstring FormatZoom(float zoom);
// 字节数转可读文案："1.2 MB"。0 表示未知
std::wstring FormatBytes(uint64_t bytes);

// ── 设置持久化（%APPDATA%\AiPic\settings.ini）──
struct Settings {
  // -1 = 未设置；坐标不持久化，启动居中
  int winW = -1;
  int winH = -1;
  bool winMax = false;
  // 主题名，对应 themes.xml 里的 <Theme name="...">；空串按浅色处理
  std::wstring theme = L"light";
  // 上次打开/另存的目录，作为下次文件对话框的起点
  std::wstring lastDir;
};
Settings LoadSettings();
void SaveSettings(const Settings& s);

}  // namespace util
