// 主界面：一个 Activity 持有全部控件
//
// 界面形状由 main_frame.xml 描述；这里只做三件事：装配控件、响应交互、
// 处理窗口级行为（拖放、快捷键、关闭时保存）。打开/保存的规则在 App 里，
// 像素处理在平台后端里，Activity 不做文件判断。
#pragma once

#include "AiUI/activity.h"
#include "AiUI/events.h"
#include "AiUI/widgets/photo_view.h"

#include <string>

namespace AiUI {
class Button;
class ButtonBar;
class StatusBar;
class TagView;
class ThumbnailStrip;
class View;
}  // namespace AiUI

namespace ui {

// 主窗口 Activity：XML 布局 + 控件装配
class MainActivity : public AiUI::Activity {
 public:
  MainActivity();
  ~MainActivity() override;

  // 打开一张图：App 解码像素，PhotoView 解码位图，两边各一次是刻意的
  // （像素要给另存用，位图要给绘制用，平台没有「位图取像素」的入口）。
  bool openImage(const std::wstring& path);
  void closeImage();
  // 按当前图所在的目录跳到前/后一张
  void stepImage(int delta);

  // 全屏演示：隐藏工具栏、缩略图条与状态栏，只留图片。
  // 方向键/PageUp/PageDown 切图，Esc 退出。
  void setPresenting(bool presenting);
  bool isPresenting() const { return presenting_; }

  // 另存为：把当前旋转导出成文件
  void saveAs();

 protected:
  const wchar_t* layoutName() const override { return L"main_frame"; }
  void onCreate() override;
  // 主窗口级快捷键（左右翻页、+/- 缩放、0 适应窗口）
  bool onKeyDown(const AiUI::Key& key, const AiUI::Modifiers& mods) override;
  bool onCloseRequest() override;

 private:
  // ── 装配 ──
  void attachToolBar();
  void attachHome();
  void attachPhoto();
  void attachStatusBar();
  void attachThumbs();

  // ── 状态同步 ──
  // 有图/无图决定看图层与首页谁显示
  void refreshStage();
  void refreshStatusBar();
  // 按钮可用性：没有图时缩放与另存没有意义
  void refreshToolBar();
  // 缩略图条：跟随同目录列表与当前项
  void refreshThumbs();
  void updateTitle();

  // ── 标题栏标签 ──
  // 分辨率、文件大小、第几张：三者都是「关于这张图」的规格，
  // 放进标题栏比塞状态栏更容易一眼扫到
  void attachTitleTags();
  void refreshTitleTags();

  // ── 动作 ──
  void zoomBy(float factor);
  void zoomToFit();
  void zoomToActual();
  void rotate(int quarterTurns);
  void pickAndOpen();
  AiUI::NativeHandle nativeWindow() const;

  // 控件由布局创建，这里只借用
  AiUI::ButtonBar* toolBar_ = nullptr;
  AiUI::PhotoView* photo_ = nullptr;
  AiUI::StatusBar* status_ = nullptr;
  AiUI::ThumbnailStrip* thumbs_ = nullptr;
  AiUI::View* homePane_ = nullptr;

  // 标题栏上的三个信息标签；没有图时整组隐藏
  AiUI::TagView* tagSize_ = nullptr;
  AiUI::TagView* tagBytes_ = nullptr;
  AiUI::TagView* tagIndex_ = nullptr;

  AiUI::Button* btnZoomIn_ = nullptr;
  AiUI::Button* btnZoomOut_ = nullptr;
  AiUI::Button* btnFit_ = nullptr;
  AiUI::Button* btnActual_ = nullptr;
  AiUI::Button* btnRotateLeft_ = nullptr;
  AiUI::Button* btnRotateRight_ = nullptr;
  AiUI::Button* btnPresent_ = nullptr;
  AiUI::Button* btnSaveAs_ = nullptr;

  bool presenting_ = false;
  // 演示态会临时隐藏标题栏，退出时按原值恢复
  bool titleBarVisible_ = true;
  // 进入演示前的缩放与位置：退出时原样退回，不把用户调好的视图弄丢
  AiUI::PhotoView::ViewState savedView_{};
};

// 进程内唯一的主窗口
MainActivity* mainActivity();

}  // namespace ui
