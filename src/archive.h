// 压缩包操作：打开/枚举/解压/压缩，封装 7-Zip 内核
#pragma once

#include "sevenzip.h"

#include <functional>
#include <string>
#include <vector>

namespace arc {

// 列表中的一项
struct Entry {
  std::wstring path;      // 包内完整路径
  std::wstring name;      // 显示名（末段）
  bool isDir = false;
  bool encrypted = false;
  uint64_t size = 0;      // 原始大小
  uint64_t packSize = 0;  // 压缩后大小
  uint32_t crc = 0;
  bool hasCrc = false;
  uint32_t attrib = 0;
  FILETIME mtime{};
  bool hasMtime = false;
  std::wstring method;    // 压缩算法
};

struct OpenResult {
  bool ok = false;
  std::wstring error;
  std::wstring formatName;
};

// 进度回调：返回 false 表示取消
using ProgressFn = std::function<bool(uint64_t completed, uint64_t total, const std::wstring& current)>;
// 同名询问：0=跳过 1=覆盖 2=重命名 3=全部覆盖 4=全部跳过
using OverwriteFn = std::function<int(const std::wstring& path)>;
// 口令询问
using PasswordFn = std::function<bool(std::wstring& out)>;

// 解压选项，对应 HaoZip 的“更新方式 / 覆盖方式”
struct ExtractOptions {
  enum class Overwrite { kAsk, kOverwrite, kSkip, kRename };
  enum class Update { kExtractReplace, kExtractUpdate, kNewerOnly };
  std::wstring targetDir;
  Overwrite overwrite = Overwrite::kAsk;
  Update update = Update::kExtractReplace;
  std::wstring password;
  bool keepBroken = false;
};

struct CompressOptions {
  std::wstring archivePath;
  std::wstring password;
  int level = 5;         // 0=存储 1=最快 3=快速 5=标准 7=最大 9=极限
  bool solid = true;     // 仅 7z
  std::wstring method;   // 覆盖算法名，空=按格式默认
  std::wstring formatName;  // 压缩格式显示名（如 L"TAR.GZ"），空=按扩展名推断
};

// 打开压缩包，保持句柄供枚举与解压
class Archive {
 public:
  Archive();
  ~Archive();
  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;

  OpenResult Open(const std::wstring& filePath, const std::wstring& password = L"");
  void Close();

  const std::vector<Entry>& Entries() const { return entries_; }
  const std::wstring& Path() const { return path_; }
  bool IsOpen() const { return archive_ != nullptr; }

  // indices 为空表示全部
  bool Extract(const std::vector<uint32_t>& indices, const ExtractOptions& opts,
               const ProgressFn& progress, const OverwriteFn& askOverwrite,
               const PasswordFn& askPassword, std::wstring& error);

  static bool IsArchiveFile(const std::wstring& path);

 private:
  void* stream_ = nullptr;  // z7::IInStream*
  z7::IInArchive* archive_ = nullptr;
  std::wstring path_;
  std::vector<Entry> entries_;
};

// 压缩一组路径到新压缩包
bool Compress(const std::vector<std::wstring>& sources, const CompressOptions& opts,
              const ProgressFn& progress, std::wstring& error);

}  // namespace arc
