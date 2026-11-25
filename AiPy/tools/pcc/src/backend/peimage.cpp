// 读 PE 里的 pylib 注册节。格式说明见头文件。
#include "pcc/backend/peimage.h"

#include "pcc/backend/fsutil.h"

#include <cstdint>
#include <cstring>

namespace pcc {
namespace {

constexpr std::uint16_t kPe32PlusMagic = 0x20B;

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

std::uint64_t readU64(const std::uint8_t* data) {
  return static_cast<std::uint64_t>(readU32(data)) |
         (static_cast<std::uint64_t>(readU32(data + 4)) << 32);
}

struct Section {
  std::string name;
  std::uint32_t virtualAddress = 0;
  std::uint32_t virtualSize = 0;
  std::uint32_t rawSize = 0;
  std::uint32_t rawPointer = 0;
};

}  // namespace

bool readLinkedExtensions(const std::string& path, LinkedExtensions* out,
                          std::string* error) {
  out->modules.clear();
  out->functions.clear();

  std::vector<std::uint8_t> bytes;
  if (!readFileBytes(path, &bytes)) {
    *error = "cannot read " + path;
    return false;
  }
  if (bytes.size() < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z') {
    *error = path + " is not a PE image";
    return false;
  }
  const std::uint32_t peOffset = readU32(bytes.data() + 0x3C);
  if (static_cast<std::uint64_t>(peOffset) + 24 > bytes.size()) {
    *error = path + " has a truncated PE header";
    return false;
  }
  if (std::memcmp(bytes.data() + peOffset, "PE\0\0", 4) != 0) {
    *error = path + " has no PE signature";
    return false;
  }

  const std::uint16_t sectionCount = readU16(bytes.data() + peOffset + 6);
  const std::uint16_t optionalSize = readU16(bytes.data() + peOffset + 20);
  const std::uint64_t optionalAt = static_cast<std::uint64_t>(peOffset) + 24;
  if (optionalAt + optionalSize > bytes.size()) {
    *error = path + " has a truncated optional header";
    return false;
  }
  if (readU16(bytes.data() + optionalAt) != kPe32PlusMagic) {
    *error = path + " is not a PE32+ (x64) image";
    return false;
  }
  // ImageBase 在 PE32+ 可选头的偏移 24。注册项里的字符串指针是**含基址的
  // 绝对虚拟地址**（节内容按首选基址写成），所以要先减去它才是 RVA。
  const std::uint64_t imageBase = readU64(bytes.data() + optionalAt + 24);

  const std::uint64_t sectionAt = optionalAt + optionalSize;
  std::vector<Section> sections;
  for (std::uint16_t i = 0; i < sectionCount; ++i) {
    const std::uint64_t at = sectionAt + static_cast<std::uint64_t>(i) * 40;
    if (at + 40 > bytes.size()) break;
    Section section;
    // 节名最多 8 字节，不保证以 '\0' 结尾。
    std::size_t length = 0;
    while (length < 8 && bytes[at + length] != 0) ++length;
    section.name.assign(reinterpret_cast<const char*>(bytes.data()) + at,
                        length);
    section.virtualSize = readU32(bytes.data() + at + 8);
    section.virtualAddress = readU32(bytes.data() + at + 12);
    section.rawSize = readU32(bytes.data() + at + 16);
    section.rawPointer = readU32(bytes.data() + at + 20);
    sections.push_back(section);
  }

  // 虚拟地址 → 文件偏移。指针是「ImageBase + RVA」，而节表里记的是 RVA，
  // 所以先减基址。基址不为零时忘了这一步的症状是「一个字符串都读不到」，
  // 表现为「产物里没有任何扩展」——恰好会被错误地当成正确结果。
  auto translate = [&sections, &bytes, imageBase](std::uint64_t address,
                                                  std::size_t* offset) {
    if (address < imageBase) return false;
    const std::uint64_t rva = address - imageBase;
    for (const Section& section : sections) {
      const std::uint64_t begin = section.virtualAddress;
      const std::uint32_t span =
          section.virtualSize > section.rawSize ? section.virtualSize
                                                : section.rawSize;
      if (rva < begin || rva >= begin + span) continue;
      const std::uint64_t at = section.rawPointer + (rva - begin);
      if (at >= bytes.size()) return false;
      *offset = static_cast<std::size_t>(at);
      return true;
    }
    return false;
  };

  auto readString = [&translate, &bytes](std::uint64_t address,
                                         std::string* text) {
    std::size_t offset = 0;
    if (address == 0 || !translate(address, &offset)) return false;
    std::size_t stop = offset;
    while (stop < bytes.size() && bytes[stop] != 0) ++stop;
    text->assign(reinterpret_cast<const char*>(bytes.data()) + offset,
                 stop - offset);
    return true;
  };

  auto findSection = [&sections](const char* name) -> const Section* {
    for (const Section& section : sections) {
      if (section.name == name) return &section;
    }
    return nullptr;
  };

  // 模块：每条一个指针（模块名）。
  if (const Section* registry = findSection(".aipyreg")) {
    const std::uint32_t span = registry->virtualSize < registry->rawSize
                                   ? registry->virtualSize
                                   : registry->rawSize;
    for (std::uint32_t at = 0; at + 8 <= span; at += 8) {
      const std::uint64_t name =
          readU64(bytes.data() + registry->rawPointer + at);
      std::string text;
      // 哨兵项是 {nullptr}，跳过。
      if (!readString(name, &text) || text.empty()) continue;
      out->modules.push_back(text);
    }
  }

  // 函数：每条三个指针（模块名、函数名、实现地址）。
  if (const Section* functions = findSection(".aipyfn")) {
    const std::uint32_t span = functions->virtualSize < functions->rawSize
                                   ? functions->virtualSize
                                   : functions->rawSize;
    for (std::uint32_t at = 0; at + 24 <= span; at += 24) {
      const std::uint64_t constAt = functions->rawPointer + at;
      const std::uint64_t moduleAddress = readU64(bytes.data() + constAt);
      const std::uint64_t nameAddress = readU64(bytes.data() + constAt + 8);
      const std::uint64_t implementation = readU64(bytes.data() + constAt + 16);
      // 哨兵项三个成员都是 nullptr。
      if (moduleAddress == 0 && nameAddress == 0 && implementation == 0) {
        continue;
      }
      LinkedFunction entry;
      if (!readString(moduleAddress, &entry.module)) continue;
      if (!readString(nameAddress, &entry.name)) continue;
      if (entry.module.empty() || entry.name.empty()) continue;
      out->functions.push_back(entry);
    }
  }

  return true;
}

}  // namespace pcc
