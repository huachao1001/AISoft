// 压缩包操作：打开/枚举/解压/压缩，封装 7-Zip 内核
#pragma once

#include "sevenzip.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace arc {

// 列表中的一项
struct Entry {
  std::wstring path;      // 包内完整路径
  bool isDir = false;
  bool encrypted = false;
  uint64_t size = 0;      // 原始大小
  uint64_t packSize = 0;  // 压缩后大小
  uint32_t crc = 0;
  bool hasCrc = false;
  // 修改时间：Unix 纪元毫秒。7z 内核给的是 FILETIME，跨层前就换算掉，
  // 展示与比较都只认这一种表示。
  int64_t modifiedMillis = 0;
  bool hasMtime = false;
  std::wstring method;    // 压缩算法
};

struct OpenResult {
  bool ok = false;
  std::wstring error;
  // 打开失败是因为内核要口令（而不是文件损坏/格式不支持）
  bool needsPassword = false;
};

// 进度回调：返回 false 表示取消
using ProgressFn = std::function<bool(uint64_t completed, uint64_t total, const std::wstring& current)>;
// 同名询问：0=跳过 1=覆盖 2=重命名 3=全部覆盖 4=全部跳过
using OverwriteFn = std::function<int(const std::wstring& path)>;
// 口令询问
using PasswordFn = std::function<bool(std::wstring& out)>;

// 解压选项。更新方式与覆盖方式已从界面移除：存在同名文件时一律
// 交给 askOverwrite 询问，调用方不提供回调则直接覆盖。
struct ExtractOptions {
  std::wstring targetDir;
  std::wstring password;
  // 落地时去掉包内这层公共目录前缀（用于"外层已建同名目录"的场景）
  std::wstring stripTopDir;
};

struct CompressOptions {
  std::wstring archivePath;
  std::wstring password;
  int level = 5;         // 0=存储 1=最快 3=快速 5=标准 7=最大 9=极限
  bool solid = true;     // 仅 7z
  std::wstring formatName;  // 压缩格式显示名（如 L"TAR.GZ"），空=按扩展名推断
  // 分卷大小（字节）；0 表示不分卷。分卷时输出为 xxx.001、xxx.002……
  // 7-Zip 内核本身不切卷，切分由外壳的输出流完成。
  uint64_t volumeSize = 0;
};

// 打开压缩包，保持句柄供枚举与解压
class Archive {
 public:
  Archive();
  ~Archive();
  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;
  // 可移动：嵌套预览要把当前层整体压栈，再让内层接管
  Archive(Archive&& other) noexcept;
  Archive& operator=(Archive&& other) noexcept;

  OpenResult Open(const std::wstring& filePath, const std::wstring& password = L"");
  void Close();

  const std::vector<Entry>& Entries() const { return entries_; }
  bool IsOpen() const { return archive_ != nullptr; }

  // indices 为空表示全部
  bool Extract(const std::vector<uint32_t>& indices, const ExtractOptions& opts,
               const ProgressFn& progress, const OverwriteFn& askOverwrite,
               const PasswordFn& askPassword, std::wstring& error);

  static bool IsArchiveFile(const std::wstring& path);

  // 所有条目都在同一个顶层目录下时返回该目录名，否则返回空。
  // 解压时可以用它避免多套一层同名目录。
  static std::wstring SingleTopLevelDir(const std::vector<Entry>& entries);

 private:
  void* stream_ = nullptr;  // z7::IInStream*
  z7::IInArchive* archive_ = nullptr;
  std::vector<Entry> entries_;
};

// 压缩一组路径到新压缩包
bool Compress(const std::vector<std::wstring>& sources, const CompressOptions& opts,
              const ProgressFn& progress, std::wstring& error);

}  // namespace arc
