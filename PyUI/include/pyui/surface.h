// 像素缓冲：一切绘制的最终落点
//
// 屏幕后端画进 DIB 再 blit 到窗口，离线渲染 / 像素测试直接画进这里。
// 两者共用同一段代码，所以「渲染到 Surface」与「渲染到屏幕」不会分叉。
//
// 像素格式固定 0xAARRGGBB，行优先、自上而下，stride 按 4 字节对齐。
// 这是三平台唯一约定的内存布局；后端若不原生支持，自行转换，不要改这里。
#pragma once

#include "pyui/types.h"

#include <cstdint>
#include <vector>

namespace pyui {

class Surface {
 public:
  Surface() = default;
  // 尺寸为 0 时得到无效 Surface（valid() == false）
  Surface(int width, int height);

  // 包装一块**外部**像素内存，不拷贝、不释放。
  //
  // 给窗口后端用：CreateDIBSection 给的就是一块可以直接画的 32 位内存，
  // 再拷进 vector 就等于每帧多一次整屏来回。调用方必须保证内存在
  // Surface 存续期间有效，且已按 0xAARRGGBB / 行优先 / 自上而下 布局。
  //
  // stride 是每行字节数；0 表示「紧密排列」（width * 4）。DIB 有时会按
  // 4 字节对齐补行尾，所以这个参数不能省。
  static Surface wrapExternal(std::uint32_t* pixels, int width, int height, int stride = 0);

  bool valid() const { return width_ > 0 && height_ > 0 && pixels_ != nullptr && pixelCount() > 0; }
  int width() const { return width_; }
  int height() const { return height_; }
  Rect bounds() const { return Rect{0, 0, width_, height_}; }

  // 指向第 y 行第 0 列的像素。y 越界返回 nullptr。
  std::uint32_t* row(int y) {
    return (y >= 0 && y < height_) ? pixels_ + static_cast<size_t>(y) * stridePixels_ : nullptr;
  }
  const std::uint32_t* row(int y) const {
    return (y >= 0 && y < height_) ? pixels_ + static_cast<size_t>(y) * stridePixels_ : nullptr;
  }

  std::uint32_t* pixels() { return pixels_; }
  const std::uint32_t* pixels() const { return pixels_; }
  size_t pixelCount() const {
    if (pixels_ == nullptr) return 0;
    // 外部内存可能是带行尾填充的，此时「像素数」按可寻址范围算，
    // 不能假定紧密排列——否则哈希与比较会读到别的行。
    if (stridePixels_ == width_) {
      return static_cast<size_t>(width_) * static_cast<size_t>(height_);
    }
    return static_cast<size_t>(stridePixels_) * static_cast<size_t>(height_);
  }

  // 取/放单个像素，越界静默忽略
  Color at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0;
    return row(y)[x];
  }
  void set(int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    row(y)[x] = c;
  }

  // 整块填充，忽略 alpha（用于清屏与调试底）
  void clear(Color c);

  // 用棋盘格铺底，表示「没有像素」。离线预览半透明内容时用它。
  void clearCheckerboard(Color a, Color b, int cell);

  // 与另一张 Surface 做像素级比较；返回不同的像素数。
  // 尺寸不同时直接返回 -1。
  int countDifferences(const Surface& other) const;
  // 逐像素最大分量差（含 alpha）；尺寸不同返回 -1。
  int maxChannelDifference(const Surface& other) const;

  // 计算一个稳定哈希，用于 golden 基线比对
  std::uint64_t contentHash() const;

 private:
  int width_ = 0;
  int height_ = 0;
  // 每行跨越的元素数（可能含行尾填充）。紧密排列时等于 width_。
  int stridePixels_ = 0;
  // 自有缓冲；为空表示像素在外部（wrapExternal）。
  std::vector<std::uint32_t> owned_;
  // 实际像素起点：自有缓冲时指向 owned_.data()，否则指向外部内存。
  std::uint32_t* pixels_ = nullptr;
};

}  // namespace pyui
