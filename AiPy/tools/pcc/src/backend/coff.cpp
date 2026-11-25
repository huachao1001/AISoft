// COFF 静态库的符号读取。为什么要扫两层见头文件。
#include "pcc/backend/coff.h"

#include "pcc/backend/fsutil.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>

namespace pcc {
namespace {

constexpr std::size_t kArHeaderSize = 60;

// COFF 符号表项：18 字节。名字 8 字节（前 4 字节为 0 时后 4 字节是字符串
// 表偏移），之后 value(4) / section(2) / type(2) / storageClass(1) / aux(1)。
constexpr std::size_t kCoffSymbolSize = 18;
// storage class 2 = external。
constexpr std::uint8_t kCoffExternal = 2;

std::uint16_t readU16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) |
         static_cast<std::uint16_t>(data[1] << 8);
}

std::uint32_t readU32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

// 索引成员用大端（GNU ar 的同名字段是小端，写错会把成员数读成二十亿）。
std::uint32_t readBig32(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
}

std::string trimSpaces(const std::string& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\0')) ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\0')) --end;
  return text.substr(begin, end - begin);
}

std::string baseNameOf(const std::string& path) {
  const std::size_t at = path.find_last_of("\\/");
  return at == std::string::npos ? path : path.substr(at + 1);
}

struct Member {
  std::string name;
  std::uint64_t offset;  // ar 头位置
  std::uint64_t body;    // 成员数据起点
  std::uint64_t size;
};

// 扫 ar 归档，拆出成员并解析长名表。
bool scanArchive(const std::vector<std::uint8_t>& bytes,
                 std::vector<Member>* members, std::string* longNames,
                 const Member** index, std::string* error) {
  const char magic[] = "!<arch>\n";
  if (bytes.size() < 8 ||
      std::memcmp(bytes.data(), magic, sizeof(magic) - 1) != 0) {
    *error = "not an ar archive";
    return false;
  }

  *index = nullptr;
  std::uint64_t position = 8;
  while (position + kArHeaderSize <= bytes.size()) {
    const char* header =
        reinterpret_cast<const char*>(bytes.data()) + position;
    const std::string rawName = trimSpaces(std::string(header, 16));

    // 大小字段在 48 偏移、宽 10、右对齐补空格。
    std::string sizeText;
    bool started = false;
    for (std::size_t i = 48; i < 58; ++i) {
      const char ch = header[i];
      if (ch == ' ') {
        if (started) break;
        continue;
      }
      if (ch < '0' || ch > '9') {
        *error = "malformed ar header size field";
        return false;
      }
      started = true;
      sizeText.push_back(ch);
    }
    if (sizeText.empty()) {
      *error = "empty ar header size field";
      return false;
    }
    const std::uint64_t size = std::strtoull(sizeText.c_str(), nullptr, 10);
    const std::uint64_t body = position + kArHeaderSize;
    if (body + size > bytes.size()) {
      *error = "ar member runs past end of file";
      return false;
    }

    if (rawName == "//") {
      longNames->assign(reinterpret_cast<const char*>(bytes.data()) + body,
                        static_cast<std::size_t>(size));
    } else if (rawName == "/") {
      Member entry;
      entry.name = "/";
      entry.offset = position;
      entry.body = body;
      entry.size = size;
      members->push_back(entry);
      *index = &members->back();
    } else if (rawName.size() > 1 && rawName[0] == '/' &&
               rawName.find_first_not_of("0123456789", 1) ==
                   std::string::npos) {
      // `/123`：长名表偏移。COFF 用 '\0' 分隔，GNU ar 用 "/\n"，两种都认。
      const unsigned long at = std::strtoul(rawName.c_str() + 1, nullptr, 10);
      std::size_t stop = longNames->find('\0', at);
      if (stop == std::string::npos) stop = longNames->find("/\n", at);
      if (stop == std::string::npos) stop = longNames->size();
      Member entry;
      entry.name = baseNameOf(longNames->substr(at, stop - at));
      entry.offset = position;
      entry.body = body;
      entry.size = size;
      members->push_back(entry);
    } else if (!rawName.empty()) {
      Member entry;
      entry.name = baseNameOf(rawName);
      entry.offset = position;
      entry.body = body;
      entry.size = size;
      members->push_back(entry);
    }

    position = body + size;
    if (position % 2 != 0) ++position;
  }
  return true;
}

// 解析一个 COFF obj 的外部定义符号。
void collectCoffSymbols(const std::uint8_t* data, std::uint64_t size,
                        std::vector<LibrarySymbol>* out,
                        const std::string& memberName) {
  if (size < 20) return;
  const std::uint32_t symbolTableOffset = readU32(data + 8);
  const std::uint32_t symbolCount = readU32(data + 12);
  if (symbolTableOffset == 0 || symbolCount == 0) return;

  // 字符串表紧跟在符号表之后：u32 总长度（含自身 4 字节）。
  const std::uint64_t stringsAt =
      static_cast<std::uint64_t>(symbolTableOffset) +
      static_cast<std::uint64_t>(symbolCount) * kCoffSymbolSize;
  const std::uint8_t* strings = nullptr;
  std::uint64_t stringsSize = 0;
  if (stringsAt + 4 <= size) {
    const std::uint32_t length = readU32(data + stringsAt);
    if (length >= 4 && stringsAt + length <= size) {
      strings = data + stringsAt;
      stringsSize = length;
    }
  }

  std::uint32_t index = 0;
  while (index < symbolCount) {
    const std::uint64_t at =
        static_cast<std::uint64_t>(symbolTableOffset) +
        static_cast<std::uint64_t>(index) * kCoffSymbolSize;
    if (at + kCoffSymbolSize > size) break;

    const std::uint8_t* entry = data + at;
    const std::uint16_t section = readU16(entry + 12);
    const std::uint8_t storage = entry[16];
    const std::uint8_t aux = entry[17];

    std::string name;
    if (entry[0] == 0 && entry[1] == 0 && entry[2] == 0 && entry[3] == 0) {
      // 长名：后 4 字节是字符串表内的偏移。
      const std::uint32_t offset = readU32(entry + 4);
      if (strings != nullptr && offset < stringsSize) {
        const char* text =
            reinterpret_cast<const char*>(strings) + offset;
        std::size_t limit = static_cast<std::size_t>(stringsSize - offset);
        std::size_t stop = 0;
        while (stop < limit && text[stop] != '\0') ++stop;
        name.assign(text, stop);
      }
    } else {
      std::size_t stop = 0;
      while (stop < 8 && entry[stop] != '\0') ++stop;
      name.assign(reinterpret_cast<const char*>(entry), stop);
    }

    // section > 0 表示「定义在这里」（0 是未定义引用，-1/-2 是绝对/调试）。
    if (storage == kCoffExternal && section > 0 && !name.empty()) {
      LibrarySymbol symbol;
      symbol.name = name;
      symbol.member = memberName;
      out->push_back(symbol);
    }

    index += 1 + aux;
  }
}

}  // namespace

bool readLibrarySymbols(const std::string& path,
                        std::vector<LibrarySymbol>* symbols,
                        std::string* error) {
  symbols->clear();

  std::vector<std::uint8_t> bytes;
  if (!readFileBytes(path, &bytes)) {
    *error = "cannot read " + path;
    return false;
  }

  std::vector<Member> members;
  std::string longNames;
  const Member* indexMember = nullptr;
  std::string detail;
  if (!scanArchive(bytes, &members, &longNames, &indexMember, &detail)) {
    *error = path + ": " + detail;
    return false;
  }

  // 第一层：库索引（只含被引用的符号，便宜）。
  if (indexMember != nullptr && indexMember->size >= 4) {
    const std::uint8_t* index = bytes.data() + indexMember->body;
    const std::uint32_t count = readBig32(index);
    const std::uint64_t offsetsAt = 4;
    const std::uint64_t namesAt = 4 + static_cast<std::uint64_t>(count) * 4;
    if (namesAt <= indexMember->size) {
      std::map<std::uint64_t, std::string> byOffset;
      for (const Member& member : members) {
        if (member.name != "/") byOffset[member.offset] = member.name;
      }
      std::uint64_t cursor = namesAt;
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t memberOffset =
            readBig32(index + offsetsAt + i * 4);
        if (cursor >= indexMember->size) break;
        std::size_t stop = static_cast<std::size_t>(cursor);
        while (stop < indexMember->size && index[stop] != 0) ++stop;
        LibrarySymbol symbol;
        symbol.name.assign(reinterpret_cast<const char*>(index) + cursor,
                           stop - static_cast<std::size_t>(cursor));
        const auto found = byOffset.find(memberOffset);
        if (found != byOffset.end()) symbol.member = found->second;
        symbols->push_back(symbol);
        cursor = stop + 1;
      }
    }
  }

  // 第二层：逐成员解析 COFF 符号表。
  //
  // 注册项（aipy_<模块>_fn_<函数>）没人引用，所以不在索引里；但它们必须
  // 能被点到名。只读索引的后果是静默退化成整库链接。
  for (const Member& member : members) {
    if (member.name == "/" || member.name == "//") continue;
    if (member.name.size() < 4) continue;
    if (member.name.compare(member.name.size() - 4, 4, ".obj") != 0) continue;
    collectCoffSymbols(bytes.data() + member.body, member.size, symbols,
                       member.name);
  }

  if (symbols->empty()) {
    *error = path + " exports no symbols";
    return false;
  }
  return true;
}

}  // namespace pcc
