// 通用工具：路径、字符串、格式化、设置
//
// 这里不出现平台类型：时间用 Unix 纪元毫秒，句柄走 AiUI 的平台接口。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace util {

// ── 路径 ──
std::wstring FileExtension(const std::wstring& path);        // 含点，小写
std::wstring FileStem(const std::wstring& path);            // 去目录去扩展
std::wstring BaseName(const std::wstring& path);            // 去目录
std::wstring DirName(const std::wstring& path);
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
bool PathExists(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
bool EnsureDirectory(const std::wstring& path);
std::wstring AbsolutePath(const std::wstring& path);

// 同名文件生成 "name (2).ext"，用于“自动重命名”
std::wstring MakeUniquePath(const std::wstring& path);

// 递归收集文件，relOut 为相对 baseDir 的路径
struct SourceItem {
  std::wstring fullPath;
  std::wstring relPath;
  uint64_t size = 0;
  // Unix 纪元毫秒；0 表示未知
  int64_t modifiedMillis = 0;
  // 平台属性位（只做透传，压缩时原样写回包里）；0 表示无
  uint32_t attributes = 0;
  bool isDir = false;
};
void CollectSources(const std::vector<std::wstring>& paths, std::vector<SourceItem>& out);

// ── 格式化 ──
std::wstring FormatSize(uint64_t bytes);                    // 1.5 MB
std::wstring FormatTime(int64_t millis);                    // 2026-10-26 10:15
std::wstring FormatCrc(uint32_t crc, bool has);             // 8 位大写十六进制

// ── 文本编码 ──
// 落盘与跨进程传递统一用 UTF-8：不依赖系统区域设置
std::string toUtf8(const std::wstring& text);
std::wstring fromUtf8(const std::string& text);

// ── 设置持久化（%APPDATA%\AIZip\settings.ini）──
struct Settings {
  std::wstring lastDir;
  std::wstring extractDir;
  std::wstring compressFormat = L"ZIP";   // 压缩格式显示名（如 L"ZIP" / L"TAR.GZ"）
  int compressLevel = 5;
  bool solid = true;
  bool openAfterExtract = true;
  int listSortColumn = 0;
  bool listSortAsc = true;
  // 用户在列表里拖过的列宽（物理像素，0 = 用设计值）
  int listColWidth[4] = {0, 0, 0, 0};
  int winW = -1, winH = -1;                        // -1 = 未设置；坐标不持久化，启动居中
  bool winMax = false;
};
Settings LoadSettings();
void SaveSettings(const Settings& s);

}  // namespace util
