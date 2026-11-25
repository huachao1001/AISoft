// ZIP 容器解析（只读）。
//
// 格式权威来源是 PKWARE 的 APPNOTE（ECMA-398）。这里只实现 `.pylib` 用到的
// 子集，并在遇到不认识的字段时明确拒绝——静默读错会表现为「库链进去但函数
// 查不到」，比直接报错难查得多。
#include "pcc/backend/zip.h"

#include "pcc/backend/fsutil.h"

#include <cstdio>
#include <cstring>

namespace pcc {
namespace {

constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr std::uint32_t kCentralFileHeader = 0x02014b50;
constexpr std::uint32_t kLocalFileHeader = 0x04034b50;

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return static_cast<std::uint16_t>(bytes[at]) |
         static_cast<std::uint16_t>(bytes[at + 1] << 8);
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return static_cast<std::uint32_t>(bytes[at]) |
         (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

bool hasRange(std::size_t total, std::size_t at, std::size_t length) {
  return at <= total && length <= total - at;
}

}  // namespace

std::uint32_t crc32Of(const std::uint8_t* data, std::size_t size) {
  // 标准 CRC-32（多项式 0xEDB88320）。按字节查表，够 `.pylib` 的量级用。
  static std::uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
      }
      table[i] = value;
    }
    ready = true;
  }

  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

bool ZipArchive::open(const std::vector<std::uint8_t>& bytes,
                      std::string* error) {
  bytes_ = bytes;
  entries_.clear();
  return parse(error);
}

bool ZipArchive::parse(std::string* error) {
  const std::vector<std::uint8_t>& bytes = bytes_;

  // ── 找中央目录结尾 ──
  //
  // 它后面可能跟一段注释（最长 64 KB），所以从尾部往前搜签名。
  std::size_t eocd = std::string::npos;
  if (bytes.size() >= 22) {
    const std::size_t lowest =
        bytes.size() > 22 + 0xFFFF ? bytes.size() - 22 - 0xFFFF : 0;
    for (std::size_t at = bytes.size() - 22 + 1; at-- > lowest;) {
      if (readU32(bytes, at) == kEndOfCentralDirectory) {
        eocd = at;
        break;
      }
    }
  }
  if (eocd == std::string::npos) {
    *error = "not a zip archive (no end-of-central-directory record)";
    return false;
  }
  if (!hasRange(bytes.size(), eocd, 22)) {
    *error = "truncated zip end-of-central-directory";
    return false;
  }

  const std::uint16_t count = readU16(bytes, eocd + 10);
  const std::uint32_t directorySize = readU32(bytes, eocd + 12);
  const std::uint32_t directoryOffset = readU32(bytes, eocd + 16);

  if (count == 0xFFFF || directoryOffset == 0xFFFFFFFFu ||
      directorySize == 0xFFFFFFFFu) {
    // ZIP64：`.pylib` 不会有，明确拒绝。默默当 32 位读会读到 0xFFFFFFFF
    // 这种越界偏移，症状是「归档坏了」而不是「格式不支持」。
    *error = "ZIP64 archives are not supported";
    return false;
  }
  if (!hasRange(bytes.size(), directoryOffset, directorySize)) {
    *error = "zip central directory is out of range";
    return false;
  }

  // ── 逐条读中央目录 ──
  std::size_t at = directoryOffset;
  for (std::uint16_t index = 0; index < count; ++index) {
    if (!hasRange(bytes.size(), at, 46) ||
        readU32(bytes, at) != kCentralFileHeader) {
      *error = "malformed zip central directory entry";
      return false;
    }

    ZipEntry entry;
    const std::uint16_t flags = readU16(bytes, at + 8);
    entry.method = readU16(bytes, at + 10);
    entry.crc32 = readU32(bytes, at + 16);
    entry.compressedSize = readU32(bytes, at + 20);
    entry.uncompressedSize = readU32(bytes, at + 24);
    const std::uint16_t nameLength = readU16(bytes, at + 28);
    const std::uint16_t extraLength = readU16(bytes, at + 30);
    const std::uint16_t commentLength = readU16(bytes, at + 32);
    entry.localHeaderOffset = readU32(bytes, at + 42);

    if ((flags & 0x0001) != 0) {
      *error = "encrypted zip entries are not supported";
      return false;
    }
    if (entry.compressedSize == 0xFFFFFFFFu ||
        entry.uncompressedSize == 0xFFFFFFFFu ||
        entry.localHeaderOffset == 0xFFFFFFFFu) {
      *error = "ZIP64 entries are not supported";
      return false;
    }
    if (!hasRange(bytes.size(), at + 46, nameLength)) {
      *error = "zip entry name is out of range";
      return false;
    }

    entry.name.assign(reinterpret_cast<const char*>(bytes.data()) + at + 46,
                      nameLength);
    entries_.push_back(entry);

    at += 46u + nameLength + extraLength + commentLength;
  }

  return true;
}

const ZipEntry* ZipArchive::find(const std::string& name) const {
  for (const ZipEntry& entry : entries_) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

bool ZipArchive::read(const ZipEntry& entry, std::vector<std::uint8_t>* out,
                      std::string* error) const {
  const std::vector<std::uint8_t>& bytes = bytes_;

  // 数据起点在本地头之后：本地头长度可能与中央目录不同（extra 字段各自
  // 一份），所以必须重新读一次本地头，不能拿中央目录的长度凑。
  if (!hasRange(bytes.size(), entry.localHeaderOffset, 30) ||
      readU32(bytes, entry.localHeaderOffset) != kLocalFileHeader) {
    *error = "malformed zip local header for '" + entry.name + "'";
    return false;
  }
  const std::uint16_t nameLength =
      readU16(bytes, entry.localHeaderOffset + 26);
  const std::uint16_t extraLength =
      readU16(bytes, entry.localHeaderOffset + 28);
  const std::size_t dataAt =
      entry.localHeaderOffset + 30u + nameLength + extraLength;

  if (!hasRange(bytes.size(), dataAt, entry.compressedSize)) {
    *error = "zip entry data is out of range for '" + entry.name + "'";
    return false;
  }

  out->clear();
  if (entry.method == 0) {
    const std::uint8_t* begin = bytes.data() + dataAt;
    out->assign(begin, begin + entry.compressedSize);
  } else if (entry.method == 8) {
    if (!inflateRaw(bytes.data() + dataAt, entry.compressedSize, out, error)) {
      *error = "cannot inflate '" + entry.name + "': " + *error;
      return false;
    }
  } else {
    *error = "unsupported zip compression method " +
             std::to_string(entry.method) + " for '" + entry.name + "'";
    return false;
  }

  if (out->size() != entry.uncompressedSize) {
    *error = "zip entry '" + entry.name + "' has unexpected size";
    return false;
  }
  // CRC 对不上 = 归档损坏或解压实现有 bug。这条校验是「用第三方解码器」
  // 的安全网：解出来长度对但内容错，只有 CRC 能发现。
  if (crc32Of(out->data(), out->size()) != entry.crc32) {
    *error = "zip entry '" + entry.name + "' failed CRC32 check";
    return false;
  }
  return true;
}

bool ZipArchive::readFile(const std::string& name,
                          std::vector<std::uint8_t>* out,
                          std::string* error) const {
  const ZipEntry* entry = find(name);
  if (entry == nullptr) {
    *error = "zip archive has no entry '" + name + "'";
    return false;
  }
  return read(*entry, out, error);
}

bool openZipFile(const std::string& path, ZipArchive* archive,
                 std::string* error) {
  std::vector<std::uint8_t> bytes;
  if (!readFileBytes(path, &bytes)) {
    *error = "cannot read " + path;
    return false;
  }
  return archive->open(bytes, error);
}

}  // namespace pcc
