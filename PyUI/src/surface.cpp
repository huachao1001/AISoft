#include "pyui/surface.h"

#include "pyui/id.h"

#include <algorithm>
#include <cstring>

namespace pyui {

Surface::Surface(int width, int height)
    : width_(width > 0 ? width : 0), height_(height > 0 ? height : 0) {
  if (width_ > 0 && height_ > 0) {
    owned_.assign(static_cast<size_t>(width_) * height_, 0);
    pixels_ = owned_.data();
    stridePixels_ = width_;
  }
}

Surface Surface::wrapExternal(std::uint32_t* pixels, int width, int height, int stride) {
  Surface surface;
  if (pixels == nullptr || width <= 0 || height <= 0) return surface;

  surface.width_ = width;
  surface.height_ = height;
  surface.pixels_ = pixels;
  // stride 参数是**字节数**；换算成元素数。不足一行时按紧密排列处理，
  // 否则越界读取会读到别的行，比「不认这个参数」更糟。
  const int strideBytes = stride > 0 ? stride : width * static_cast<int>(sizeof(std::uint32_t));
  const int stridePixels = strideBytes / static_cast<int>(sizeof(std::uint32_t));
  surface.stridePixels_ = stridePixels >= width ? stridePixels : width;
  return surface;
}

void Surface::clear(Color c) {
  if (pixels_ == nullptr || width_ <= 0 || height_ <= 0) return;
  // 逐行填而不是整块 memset：外部内存可能带行尾填充，
  // 整块填会把填充区也写掉（DIB 里那部分不属于本图，但也没有害处）。
  // 逐行更准确地表达「只清本图的像素」。
  for (int y = 0; y < height_; ++y) {
    std::fill_n(row(y), static_cast<size_t>(width_), c);
  }
}

void Surface::clearCheckerboard(Color a, Color b, int cell) {
  if (pixels_ == nullptr || width_ <= 0 || height_ <= 0) return;
  if (cell <= 0) {
    clear(a);
    return;
  }
  for (int y = 0; y < height_; ++y) {
    std::uint32_t* out = row(y);
    const bool oddRow = ((y / cell) & 1) != 0;
    for (int x = 0; x < width_; ++x) {
      const bool oddCol = ((x / cell) & 1) != 0;
      out[x] = (oddRow != oddCol) ? b : a;
    }
  }
}

int Surface::countDifferences(const Surface& other) const {
  if (width_ != other.width_ || height_ != other.height_) return -1;
  if (!valid() || !other.valid()) return 0;
  int count = 0;
  for (int y = 0; y < height_; ++y) {
    const std::uint32_t* a = row(y);
    const std::uint32_t* b = other.row(y);
    for (int x = 0; x < width_; ++x) {
      if (a[x] != b[x]) ++count;
    }
  }
  return count;
}

int Surface::maxChannelDifference(const Surface& other) const {
  if (width_ != other.width_ || height_ != other.height_) return -1;
  if (!valid() || !other.valid()) return 0;
  int worst = 0;
  for (int y = 0; y < height_; ++y) {
    const std::uint32_t* a = row(y);
    const std::uint32_t* b = other.row(y);
    for (int x = 0; x < width_; ++x) {
      const std::uint32_t p = a[x];
      const std::uint32_t q = b[x];
      for (int shift = 0; shift < 32; shift += 8) {
        const int da = static_cast<int>((p >> shift) & 0xFFu);
        const int db = static_cast<int>((q >> shift) & 0xFFu);
        const int d = da > db ? da - db : db - da;
        if (d > worst) worst = d;
      }
    }
  }
  return worst;
}

std::uint64_t Surface::contentHash() const {
  // 尺寸也参与哈希：两张内容不同但同色、尺寸不同的图不应该撞在一起
  std::uint64_t hash = detail::kFnvOffset64;
  auto mixByte = [&hash](std::uint8_t byte) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= detail::kFnvPrime64;
  };
  for (int shift = 0; shift < 32; shift += 8) mixByte(static_cast<std::uint8_t>(width_ >> shift));
  for (int shift = 0; shift < 32; shift += 8) mixByte(static_cast<std::uint8_t>(height_ >> shift));

  // 逐行哈希本图的有效像素，跳过行尾填充：否则同一张图用不同 stride
  // 包装会得到不同哈希，基线比对就失效了。
  for (int y = 0; y < height_; ++y) {
    const std::uint32_t* line = row(y);
    if (line == nullptr) continue;
    const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(line);
    const size_t count = static_cast<size_t>(width_) * sizeof(std::uint32_t);
    for (size_t i = 0; i < count; ++i) mixByte(bytes[i]);
  }
  return hash;
}

}  // namespace pyui
