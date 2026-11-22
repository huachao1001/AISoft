#include "pyui/types.h"

#include "pyui/id.h"

namespace pyui {
namespace {

int gDesignDpi = 96;

}  // namespace

int designDpi() { return gDesignDpi; }

void setDesignDpi(int dpi) { gDesignDpi = dpi > 0 ? dpi : 96; }

int scaleDesign(int value) {
  // 四舍五入而不是截断：150% 屏下 1 设计像素 = 1.5 物理像素，
  // 截断会让所有奇数设计值都少半像素，一列控件累积下来就差出好几像素。
  const long long scaled = static_cast<long long>(value) * gDesignDpi;
  if (scaled >= 0) return static_cast<int>((scaled + 48) / 96);
  return -static_cast<int>((-scaled + 48) / 96);
}

std::uint32_t hashBytes32(const char* data, std::size_t size) {
  std::uint32_t hash = detail::kFnvOffset32;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(data[i]));
    hash *= detail::kFnvPrime32;
  }
  return hash == 0u ? 1u : hash;
}

std::uint64_t hashBytes64(const char* data, std::size_t size) {
  std::uint64_t hash = detail::kFnvOffset64;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i]));
    hash *= detail::kFnvPrime64;
  }
  return hash;
}

}  // namespace pyui
