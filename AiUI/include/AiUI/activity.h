// Activity：一个界面的持有者
//
// 对应 Android 的 Activity：子类只声明用哪份 XML 布局，构造与销毁时机由基类
// 把握，界面上的控件通过 findView 取。和 Activity 不同的是没有 Intent、没有
// 任务栈——桌面程序通常就是一个主窗口加若干模态对话框。
//
// 典型用法：
//   class MainActivity : public AiUI::Activity {
//    protected:
//     const wchar_t* layoutName() const override { return L"main_frame"; }
//     void onCreate() override {
//       tree_ = findView<TreeView>("tree");
//       // 此时布局已建好、id 已生效，可以挂回调了
//     }
//    private:
//     TreeView* tree_ = nullptr;   // 成员持有，不再需要文件级全局
//   };
//
//   MainActivity act;
//   act.create(nullptr);
//   act.show();
//
// 复杂逻辑不要塞进子类：把业务抽成普通类，Activity 只负责把控件交给它们，
// 界面变化时再调一下它们的 Refresh。这样 Activity 始终只有装配几行。
#pragma once

#include "AiUI/platform.h"
#include "AiUI/types.h"
#include "AiUI/view.h"
#include "AiUI/window.h"

#include <memory>
#include <string>

namespace AiUI {

// 布局来源：XML 名或直接给的视图树。子类只需实现一种。
class Activity {
 public:
  Activity();
  virtual ~Activity();

  Activity(const Activity&) = delete;
  Activity& operator=(const Activity&) = delete;

  // ── 主题 ──
  // 全局默认主题：所有 Activity 创建时都从这里取。
  // nullptr 表示浅色。适合在启动时按用户设置调用一次。
  static void setDefaultTheme(const Theme* theme);
  static const Theme* defaultThemePtr();

  // 本界面单独指定主题（覆盖全局默认）。传给 Window 的是指针，
  // 所以对象生命周期要覆盖窗口——通常用 kDarkTheme 或 ThemeRegistry 里的对象。
  void setTheme(const Theme* theme);
  const Theme* appTheme() const { return theme_; }
  // 便捷入口：在浅色/深色之间切换；创建前调用会存下来，创建后立即生效
  void setDarkMode(bool dark);
  bool isDarkMode() const { return darkMode_; }

  // ── 生命周期（子类覆盖）──
  // 布局对应的 XML 文件名（不含扩展名）。返回空表示用 createView() 自建。
  virtual const wchar_t* layoutName() const { return nullptr; }
  // 不用 XML 时自建根视图；返回的对象由 Activity 接管
  virtual View* createView() { return nullptr; }
  // 布局已建好、已挂上 id，可以 findView 并挂回调
  virtual void onCreate() {}
  // 焦点视图没消费的按键；返回 true 表示已消费
  virtual bool onKeyDown(const Key& key, const Modifiers& mods) {
    (void)key;
    (void)mods;
    return false;
  }
  // 窗口即将销毁；此时控件仍在，可以保存状态
  virtual void onDestroy() {}

  // ── 窗口外观（子类可在 onCreate 前调整）──
  void setTitle(const std::wstring& title);
  void setStyle(const WindowStyle& style);
  void setMinSize(int width, int height);
  // Esc 是否关闭窗口；对话框用 true
  void setEscapeCloses(bool closes);

  // ── 阶段 ──
  // 建布局与窗口。owner 为主窗口时可为空。
  bool create(NativeHandle owner);
  // 创建后、显示前设置外框位置与客户区尺寸
  void setInitialBounds(const Rect& bounds);
  void show(bool visible = true);
  // 模态运行，直到窗口关闭。返回前会触发 onDestroy。
  void showModal(NativeHandle owner);
  void close();
  bool isOpen() const;

  // ── 取界面 ──
  Window* window() const { return window_.get(); }
  View* contentView() const;
  // 按 XML id 找控件；布局尚未建好时返回 nullptr
  View* findView(const char* id) const;
  template <typename T>
  T* findView(const char* id) const {
    return dynamic_cast<T*>(findView(id));
  }
  // 布局根视图换成具体类型时用
  template <typename T>
  T* rootAs() const {
    return dynamic_cast<T*>(contentView());
  }

  int dpi() const;

 protected:
  // 窗口关闭前的最后一次机会；返回 false 可阻止关闭（如"还有未保存的修改"）
  virtual bool onCloseRequest() { return true; }

 private:
  void buildContent();

  std::unique_ptr<Window> window_;
  // 本界面的主题；nullptr 表示跟随全局默认
  const Theme* theme_ = nullptr;
  bool darkMode_ = false;
  std::wstring title_;
  WindowStyle style_;
  int minWidth_ = 0;
  int minHeight_ = 0;
  bool escapeCloses_ = false;
  bool created_ = false;
};

}  // namespace AiUI
