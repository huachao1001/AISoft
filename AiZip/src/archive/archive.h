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
  // 本次解压显式指定的口令。留空就用 Archive 记住的那一个（Password()），
  // 界面不必每层自己维护一份。
  std::wstring password;
  // 落地时去掉包内这层公共目录前缀（用于"外层已建同名目录"的场景）
  std::wstring stripTopDir;
};

// 解压的三种结局。取消必须和失败分开表达：取消是用户自己按的（关掉口令
// 输入框、点进度窗的取消），调用方应当静默收场；失败才需要弹错误框。
//
// 以前用「返回 false + error 为空」表示取消，调用方看到空串就顺手补一句
// 「无法解出该内容」，于是用户明明自己取消了，还会挨一个弹窗。
struct ExtractResult {
  bool ok = false;
  bool canceled = false;
  std::wstring error;  // 仅失败时有意义
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
  // 这个包实际用过的口令（打开阶段给的，或解压阶段问到的）。
  //
  // 记在 Archive 里而不是界面上：一层包一个对象，嵌套预览压栈时口令跟着
  // 一起走，天然按层隔离。加密包能读出目录时内核打开阶段不要口令，第一次
  // 真解数据才要，不记下来每点一个文件都要用户重输一遍。
  const std::wstring& Password() const { return password_; }
  // 真正把包打开的那个 handler 对应的格式显示名（如 L"ZIP"）。
  // 没开包或该 handler 没有可读名字时返回空串。
  std::wstring FormatName() const;

  // indices 为空表示全部。
  //
  // 取消与失败分开返回：ExtractResult::canceled 为真时调用方必须静默收场。
  ExtractResult Extract(const std::vector<uint32_t>& indices, const ExtractOptions& opts,
                        const ProgressFn& progress, const OverwriteFn& askOverwrite,
                        const PasswordFn& askPassword);

  static bool IsArchiveFile(const std::wstring& path);

  // 所有条目都在同一个顶层目录下时返回该目录名，否则返回空。
  // 解压时可以用它避免多套一层同名目录。
  static std::wstring SingleTopLevelDir(const std::vector<Entry>& entries);

 private:
  void* stream_ = nullptr;  // z7::IInStream*
  z7::IInArchive* archive_ = nullptr;
  std::vector<Entry> entries_;
  // 打开成功的那个 handler。格式标签要用它，而不是猜扩展名。
  GUID handlerClsid_ = {};
  bool hasHandlerClsid_ = false;
  // 最近一次成功用过的口令；空表示还没用过口令
  std::wstring password_;
};

// 压缩一组路径到新压缩包
bool Compress(const std::vector<std::wstring>& sources, const CompressOptions& opts,
              const ProgressFn& progress, std::wstring& error);

}  // namespace arc
