// 图标标识与图片句柄
#pragma once

#include "AiUI/types.h"

#include <cstdint>
#include <vector>

namespace AiUI {

// 图片在目标区域内的适配方式
enum class ImageScale {
  kFit,      // 等比缩放到完全放得下（默认）
  kFill,     // 等比铺满，超出部分裁掉
  kStretch,  // 拉伸到目标大小
  kCenter,   // 原始大小居中
};

// 界面用到的矢量图标
enum class IconId {
  kNone,
  kRefresh,
  kExtract,      // 提取选中
  kExtractAll,   // 全部解压
  kAdd,          // 添加文件
  kTrash,        // 删除
  kSave,         // 保存 / 导出
  kArrowBack,
  kChevron,      // 面包屑分隔
  kChevronUp,    // 表头升序
  kChevronDown,  // 表头降序
  kHome,
  kFolder,
  kFile,
  kSearch,
  kCheck,  // 复选项打勾
  kEye,    // 显示密码（明文）
  kEyeOff, // 隐藏密码（掩码）
  // 图片查看与编辑
  kZoomIn,      // 放大
  kZoomOut,     // 缩小
  kFitWindow,   // 适应窗口
  kRotateLeft,  // 逆时针旋转
  kRotateRight, // 顺时针旋转
  kImage,       // 图片（空状态占位）
  // 安装器维护页
  kRepair,      // 修复（扳手）
  kUninstall,   // 卸载（回收站）
  kHeart,       // 挽留（心形）
  // 标题栏系统按钮
  kMinimize,
  kMaximize,
  kRestore,
  kClose,
};

// 位图句柄：由平台后端从系统图标转换而来，绘制时按值传递
class Image {
 public:
  Image() = default;
  explicit Image(void* handle) : handle_(handle) {}
  void* handle() const { return handle_; }
  bool isValid() const { return handle_ != nullptr; }

 private:
  void* handle_ = nullptr;
};

// ── 像素图 ──
//
// 界面上的 Image 只是个后端句柄，取不到内容。需要真正读写像素的场合
// （图像编辑、另存为）走这里：解码到内存里的一段 ARGB，A 在高位，
// 与 Image 的 handle 无关，编解码都在平台后端里完成。
struct RgbaImage {
  int width = 0;
  int height = 0;
  // 长度 = width * height，每像素 0xAARRGGBB，行优先、自上而下
  std::vector<uint32_t> pixels;

  bool isEmpty() const { return width <= 0 || height <= 0 || pixels.empty(); }
};

// 保存时用的编码格式。上层按目标文件后缀选一个，具体编码器由后端决定，
// 这样界面层不必出现任何平台图像库的名字。
enum class ImageFormat {
  kPng,
  kJpeg,
  kBmp,
};

// 取文件的系统关联图标；句柄由库缓存持有，调用方不要释放。
// 返回无效 Image 表示没有可用图标，调用方应退回矢量图标。
//
// pixelSize 是要画出来的设备像素边长。系统图标有多个档位（16/20/24/32/40/
// 48/64/96/128/256），只取 16×16 再放大到 48 会糊成一片——维护页的应用图标
// 就是这么糊的。这里按需要的边长向 shell 要最接近的档位，宁可略大让 GDI+
// 缩下来，也不要小了再拉伸：缩小是抗锯齿重采样，放大只能插值猜像素。
Image fileIcon(const std::wstring& path, bool isDirectory, int pixelSize = 0);

// 释放全部缓存图标；宿主退出前调用一次
void releaseFileIcons();

}  // namespace AiUI
