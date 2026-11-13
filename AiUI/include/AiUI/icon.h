// 图标标识与图片句柄
#pragma once

#include "AiUI/types.h"

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

// 取文件的系统关联图标；句柄由库缓存持有，调用方不要释放。
// 返回无效 Image 表示没有可用图标，调用方应退回矢量图标。
Image fileIcon(const std::wstring& path, bool isDirectory);

// 释放全部缓存图标；宿主退出前调用一次
void releaseFileIcons();

}  // namespace AiUI
