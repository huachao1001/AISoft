// 读 `.pylib`：一个 ZIP 归档（deflate 存储）。
//
// ── 为什么自己解析 ZIP ──
//
// 7-Zip 的 Archive 层（`IInArchive`）要注册一整张 codec 表、依赖十几 MB 的
// 库；而 `.pylib` 就是一个结构固定的 ZIP：目录在尾部、条目是 stored 或
// deflate。自己按 ECMA-398 解析中央目录只有两百行，且错误可控。
//
// 解压不自己写：deflate 的位数与回溯拷贝是经典的正确性陷阱，7-Zip 的
// `DeflateDecoder` 已经在 third_party/7z 里（见 README），直接调它。
//
// ── 只支持必要的子集 ──
//
//   stored (0) 与 deflate (8)：zipfile 默认只写这两种
//   ZIP64：构建期产生的包不会超过 4 GB，但显式拒绝而不是默默读错
//   加密、分卷：不支持，遇到就报错
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pcc {

// 归档里的一个条目（来自中央目录）。
struct ZipEntry {
  std::string name;          // 归档内路径，`/` 分隔
  std::uint16_t method = 0;  // 0 = stored, 8 = deflate
  std::uint32_t crc32 = 0;
  std::uint64_t compressedSize = 0;
  std::uint64_t uncompressedSize = 0;
  std::uint64_t localHeaderOffset = 0;
};

class ZipArchive {
 public:
  // 从内存里的归档字节解析中央目录。失败返回 false，原因在 error。
  //
  // 内部**拷贝**一份字节：调用方的 vector 可能先于归档析构（`openZipFile`
  // 里就是这种情况），保存指针会变成悬垂引用。
  bool open(const std::vector<std::uint8_t>& bytes, std::string* error);

  const std::vector<ZipEntry>& entries() const { return entries_; }

  // 找条目。没有返回 nullptr。
  const ZipEntry* find(const std::string& name) const;

  // 取出条目内容（会解压）。失败返回 false。
  bool read(const ZipEntry& entry, std::vector<std::uint8_t>* out,
            std::string* error) const;

  // 便捷：按名字取。没有这个条目时返回 false。
  bool readFile(const std::string& name, std::vector<std::uint8_t>* out,
                std::string* error) const;

 private:
  bool parse(std::string* error);

  std::vector<std::uint8_t> bytes_;
  std::vector<ZipEntry> entries_;
};

// 从文件读并打开。
bool openZipFile(const std::string& path, ZipArchive* archive,
                 std::string* error);

// 解一段 raw deflate 数据。单独暴露是因为 VM 侧将来也要用它。
bool inflateRaw(const std::uint8_t* data, std::size_t size,
                std::vector<std::uint8_t>* out, std::string* error);

// CRC32（ZIP 用标准多项式）。用来校验解压结果。
std::uint32_t crc32Of(const std::uint8_t* data, std::size_t size);

}  // namespace pcc
