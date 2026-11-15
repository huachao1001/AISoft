// 应用状态与实际动作
//
// 界面只调这里的函数，自己不做路径判断与像素处理；「打开/保存」的规则
// 因此只有一处，Activity 依旧只负责装配控件。
#include "app.h"

#include "util.h"
#include "AiUI/platform.h"

#include <algorithm>
#include <iterator>

App g_app;

namespace {

AiUI::Platform* platform() { return AiUI::platform(); }

// 当前图的原始像素（未旋转）。另存、旋转都以它为基准，
// 避免每转一次就重采样一遍、画质逐次下降。
AiUI::RgbaImage g_pixels;
std::wstring g_path;
bool g_hasImage = false;
int g_rotation = 0;

// 同目录的可显示图片，按文件名排好；左右键翻页靠它的顺序
std::vector<std::wstring> g_siblings;
size_t g_siblingIndex = 0;
bool g_siblingsValid = false;

// 顺时针旋转 90° 的整数倍，按像素重排。
// 不靠绘制变换做：另存为要把旋转结果写进文件，那一步没有任何绘制上下文。
AiUI::RgbaImage rotatePixels(const AiUI::RgbaImage& src, int quarterTurns) {
  const int turns = ((quarterTurns % 4) + 4) % 4;
  if (turns == 0 || src.isEmpty()) return src;

  const int sw = src.width;
  const int sh = src.height;
  const bool swap = (turns % 2) == 1;

  AiUI::RgbaImage dst;
  dst.width = swap ? sh : sw;
  dst.height = swap ? sw : sh;
  dst.pixels.resize(static_cast<size_t>(dst.width) * static_cast<size_t>(dst.height));

  for (int y = 0; y < dst.height; ++y) {
    for (int x = 0; x < dst.width; ++x) {
      int sx = 0;
      int sy = 0;
      switch (turns) {
        case 1:  // 顺时针：目标第 x 列来自源第 x 行的末尾
          sx = y;
          sy = sh - 1 - x;
          break;
        case 2:
          sx = sw - 1 - x;
          sy = sh - 1 - y;
          break;
        default:  // 3：逆时针
          sx = sw - 1 - y;
          sy = x;
          break;
      }
      dst.pixels[static_cast<size_t>(y) * dst.width + x] =
          src.pixels[static_cast<size_t>(sy) * sw + sx];
    }
  }
  return dst;
}

// 目录内容随时会变（别处删了文件、新存了一张），每次打开都重建。
// 与当前图完全同名的条目按系统路径规则比较，其它一律按名字排序，
// 这样用户在资源管理器里看到的顺序与左右键的顺序一致。
void loadSiblings(const std::wstring& path) {
  g_siblings.clear();
  g_siblingIndex = 0;
  g_siblingsValid = false;

  const std::wstring dir = util::DirName(path);
  if (dir.empty()) return;

  for (const auto& entry : platform()->listDirectory(dir, true)) {
    if (entry.isDir || !util::IsImageFile(entry.name)) continue;
    g_siblings.push_back(entry.fullPath);
  }
  std::sort(g_siblings.begin(), g_siblings.end(),
            [](const std::wstring& a, const std::wstring& b) {
              return platform()->compareText(util::BaseName(a), util::BaseName(b)) < 0;
            });

  const std::wstring full = util::AbsolutePath(path);
  for (size_t i = 0; i < g_siblings.size(); ++i) {
    if (platform()->equalsPath(g_siblings[i], full)) {
      g_siblingIndex = i;
      g_siblingsValid = true;
      return;
    }
  }
}

}  // namespace

bool AppOpenImage(const std::wstring& path) {
  if (path.empty() || !util::PathExists(path)) return false;

  // 像素解码失败就不动现状：用户点错一个文件不该把当前图弄丢
  AiUI::RgbaImage pixels;
  if (!platform()->loadImagePixels(path, pixels)) return false;

  g_pixels = std::move(pixels);
  g_path = util::AbsolutePath(path);
  g_hasImage = true;
  // 换图意味着换内容：旋转归零，否则新图会顶着上一张的角度打开
  g_rotation = 0;

  g_app.settings.lastDir = util::DirName(g_path);
  loadSiblings(g_path);
  return true;
}

void AppCloseImage() {
  g_pixels = AiUI::RgbaImage{};
  g_path.clear();
  g_hasImage = false;
  g_rotation = 0;
  g_siblings.clear();
  g_siblingIndex = 0;
  g_siblingsValid = false;
}

const std::wstring& AppImagePath() { return g_path; }

bool AppHasImage() { return g_hasImage; }

int AppRotation() { return g_rotation; }

void AppSetRotation(int quarterTurns) { g_rotation = ((quarterTurns % 4) + 4) % 4; }

AiUI::RgbaImage AppExportPixels() { return rotatePixels(g_pixels, g_rotation); }

AiUI::Size AppImageSize() {
  return AiUI::Size{g_pixels.width, g_pixels.height};
}

bool AppReadPixelRegion(const AiUI::Rect& region, AiUI::RgbaImage& out) {
  out = AiUI::RgbaImage{};
  if (g_pixels.isEmpty()) return false;

  const AiUI::Rect clipped =
      region.intersect(AiUI::Rect{0, 0, g_pixels.width, g_pixels.height});
  if (clipped.isEmpty()) return false;

  out.width = clipped.width();
  out.height = clipped.height();
  out.pixels.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height));
  for (int y = 0; y < out.height; ++y) {
    const size_t src = static_cast<size_t>(clipped.top + y) *
                           static_cast<size_t>(g_pixels.width) +
                       static_cast<size_t>(clipped.left);
    std::copy_n(g_pixels.pixels.begin() + static_cast<std::ptrdiff_t>(src), out.width,
                out.pixels.begin() + static_cast<std::ptrdiff_t>(y) * out.width);
  }
  return true;
}

const std::vector<std::wstring>& AppSiblingImages() { return g_siblings; }

bool AppStepImage(int delta) {
  if (!g_siblingsValid || g_siblings.empty() || delta == 0) return false;
  const long long next = static_cast<long long>(g_siblingIndex) + delta;
  if (next < 0 || next >= static_cast<long long>(g_siblings.size())) return false;
  return AppOpenImage(g_siblings[static_cast<size_t>(next)]);
}
