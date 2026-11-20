// 编辑器主界面
//
// 界面形状由 editor_frame.xml 描述。这里只做装配与交互：编辑状态（当前工具、
// 裁剪框、目标尺寸）放在 EditSession 里，像素运算在 editor::pixels 里，
// 这个类不碰任何一个像素。
#pragma once

#include "AiUI/activity.h"
#include "AiUI/events.h"
#include "AiUI/icon.h"
#include "AiUI/widgets/photo_view.h"

#include <string>
#include <vector>

namespace AiUI {
class Button;
class LinearLayout;
class Slider;
class TextView;
class View;
}  // namespace AiUI

namespace ui {

// 当前在调哪一类参数。面板内容按它切换，图片预览始终是同一张。
enum class Tool { kCrop, kResize };

class EditorActivity : public AiUI::Activity {
 public:
  EditorActivity();
  ~EditorActivity() override;

  bool openImage(const std::wstring& path);
  void closeImage();
  void saveAs();

  // 当前工具：切换后重建参数面板
  void setTool(Tool tool);
  Tool tool() const { return tool_; }

  // 预览用的「已应用」像素：退出重进、重置都以它为基准
  const AiUI::RgbaImage& pixels() const { return pixels_; }

  // 离屏快照：布局后整棵树画一遍。开发时核对界面靠它
  void paintForSnapshot(AiUI::Painter& painter, const AiUI::Rect& area);

 protected:
  const wchar_t* layoutName() const override { return L"editor_frame"; }
  void onCreate() override;
  bool onKeyDown(const AiUI::Key& key, const AiUI::Modifiers& mods) override;

 private:
  // ── 装配 ──
  void attachTabs();
  void attachActions();
  void attachHome();

  // ── 面板构建 ──
  void rebuildPanel();
  void buildCropPanel();
  void buildResizePanel();
  // 加一行「标题 + 滑块 + 数值」，返回滑块供调用方接回调
  AiUI::Slider* addSliderRow(const std::wstring& label, float value,
                             const std::wstring& valueText, AiUI::LinearLayout* body);
  AiUI::TextView* addLabel(const std::wstring& text, bool muted,
                           AiUI::LinearLayout* body);

  // ── 状态同步 ──
  void refreshStage();
  void refreshPreview();
  void refreshActions();
  // 把像素交给 PhotoView（内部管理 Image 句柄的释放）
  void presentPixels(const AiUI::RgbaImage& pixels);

  // ── 动作 ──
  void applyTool();
  void resetTool();
  void pickAndOpen();
  AiUI::NativeHandle nativeWindow() const;

  // 当前图的原始像素（未经任何编辑）：重置时从这里回退
  AiUI::RgbaImage original_;
  // 已经应用到预览上的像素
  AiUI::RgbaImage pixels_;
  std::wstring path_;

  Tool tool_ = Tool::kCrop;

  // 裁剪：以 0..1 的比例表示，换图或改目标比例时仍然有意义
  float cropLeft_ = 0.0f;
  float cropTop_ = 0.0f;
  float cropRight_ = 1.0f;
  float cropBottom_ = 1.0f;

  // 缩放：宽高直接用像素值，0 表示「跟随另一边等比」
  int resizeWidth_ = 0;
  int resizeHeight_ = 0;

  AiUI::PhotoView* photo_ = nullptr;
  AiUI::View* homePane_ = nullptr;
  AiUI::LinearLayout* toolTabs_ = nullptr;
  AiUI::LinearLayout* panelBody_ = nullptr;
  AiUI::Button* btnApply_ = nullptr;
  AiUI::Button* btnReset_ = nullptr;
  AiUI::Button* btnHomeOpen_ = nullptr;

  // 工具标签：与 Tool 一一对应
  std::vector<AiUI::Button*> tabs_;

  // 当前交给 PhotoView 的位图句柄：换预览时必须先释放旧的
  AiUI::Image previewImage_;
};

}  // namespace ui
