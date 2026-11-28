// 脚本运行期：Python 源码在构建期编译成 C++ 之后，调用到的就是这一层
//
// ── 为什么不是内嵌解释器 ──
//
//   1. 体积。CPython 解释器 + 标准库压缩后也有 4~8 MB，而整个库的目标是
//      「产物接近纯 C++」。构建期转译不占一个字节。
//   2. 速度。转译后每个语句都是直接的 C++ 调用，没有字节码分派、没有
//      名字查表、没有引用计数。宿主循环与控件绘制因此完全不受脚本影响。
//   3. 部署。产物里没有 .py 文件、没有 zip 包、没有 import 路径问题。
//
// ── 前端用 CPython 的 ast ──
//
// 解析不自己写：手写词法/语法分析器是「与 CPython 语法定义的第二份实现」，
// 永远追不上，而且报错位置会指错行。pyuic 用标准库 ast 拿语法树，因此
// **语法覆盖范围等于当前 CPython**，报错行号也来自 CPython 本身。
// 我们只保留无法外包的那一半：AST → C++ 的代码生成。
//
// 于是这一层就是「代码生成的目标」：每个 py:: 函数对应一个脚本里的写法。
// 控件的绘制与布局仍然完全在 C++ 侧，脚本碰不到 Painter。
#pragma once

#include "pyui/events.h"
#include "pyui/menu.h"
#include "pyui/py_value.h"
#include "pyui/types.h"

#include <functional>
#include <string>
#include <vector>

namespace pyui {

class View;
class ViewGroup;
class AppWindow;

namespace py {

// 窗口与程序级描述。脚本在模块顶层赋值 TITLE / WIDTH / … 时由 pyuic 填进
// 这里，于是 main.cpp 不需要重复一遍这些数字——「界面的形状」只有一份定义
// 来源：脚本 + XML。
struct AppSpec {
  std::string title = "PyUI";
  std::string layout;  // XML 文件名（不带扩展名），来自 LAYOUT
  int width = 640;
  int height = 480;
  int minWidth = 320;
  int minHeight = 240;
  // 动画帧间隔（毫秒）；0 表示不跑帧定时器
  int frameInterval = 0;
  std::string theme = "light";
  // 是否画内置标题栏。false 表示不画那条标题栏，内容铺满窗口；
  // 窗口本身仍是自绘的（系统标题栏从不出现），要拖动就在布局里写 role="caption"。
  bool customTitleBar = true;
  bool resizable = true;

  // 文件关联：progId 非空时才登记。后缀不含点，如 {"pynote"}
  std::string progId;
  std::string fileDescription;
  std::vector<std::string> extensions;
  // 托盘图标悬停文字；空表示不挂托盘
  std::string trayTooltip;
};

// 脚本实例。生成的类继承它，成员函数就是 Python 里的 def。
//
// 为什么是「一个实例持有一份状态」而不是全局变量：同一个程序可以同时开两个
// 窗口、跑两份同一脚本，各自有独立状态。全局变量做不到，而用户第二次想要
// 「再来一个」的时候再改就晚了。
class Script {
 public:
  virtual ~Script() = default;

  // 挂上布局根。生成的 build() 会先调它，再跑模块级语句。
  void setRoot(ViewGroup* root) { root_ = root; }
  ViewGroup* rootView() const { return root_; }

  void setWindow(AppWindow* window) { window_ = window; }
  AppWindow* window() const { return window_; }

  // 按 XML 里的 id 找控件；找不到返回 None 而不是空指针，
  // 这样脚本里 `find("x") is None` 能写出来，而不会在访问成员时崩。
  Value find(const Value& id) const;

  // ── 脚本覆写点 ──
  // 生成的 describe() 填窗口描述；脚本没写对应常量时保持默认值。
  virtual void describe(AppSpec& spec) { (void)spec; }
  // 模块级语句（变量初值、常量表）在窗口建出来之前跑一次。
  //
  // 为什么不放进 build()：Python 的模块体在 import 时就执行完，而 build()
  // 是「界面已挂好」之后才调的钩子。两者混在一起会让「变量初值」依赖
  // 窗口已经存在，于是 find() 在初始化阶段就有了意义——那是错的。
  virtual void initState() {}
  // 布局挂好之后、进入消息循环之前跑一次
  virtual void build() {}
  // 帧定时器：只有 wantsFrameTimer() 为真时才会被调用
  virtual void onFrame() {}
  virtual bool wantsFrameTimer() const { return false; }
  virtual void onResize(int width, int height) {
    (void)width;
    (void)height;
  }
  virtual bool onKeyDown(const Key& key, const Modifiers& mods) {
    (void)key;
    (void)mods;
    return false;
  }
  // 可打印字符（含中文、运算符）。返回 true 表示已消费。
  virtual bool onChar(std::uint32_t codepoint) {
    (void)codepoint;
    return false;
  }
  virtual bool onCloseRequest() { return false; }
  virtual void onFileDrop(const std::vector<std::string>& paths) { (void)paths; }
  // 命令行带上关联文件时（双击打开）
  virtual void openFile(const std::string& path) { (void)path; }

  // 托盘：左键点击；右键菜单内容每次现取（与 TrayHost 的约定一致）
  virtual void onTrayActivate() {}
  virtual std::vector<MenuItem> trayMenu() { return {}; }
  virtual void onTrayCommand(const std::string& id) { (void)id; }

  // ── 平台子系统（脚本门面）──
  //
  // 脚本没有类型信息，手上只有 py::Value，所以这一组接收 Value 而不是
  // std::string / vector<MenuItem>。名字刻意与平台层的强类型版本不同，
  // 避免同名重载在 `const Value&` 与 `const std::string&` 之间产生二义。
  Value trayShow(const Value& tooltip);
  Value trayHide();
  Value traySetTooltip(const Value& tooltip);
  // 弹在鼠标当前位置，返回选中项 id（取消为空串）
  Value menuPopup(const Value& items);
  // 关联登记；成功返回 None，失败返回错误描述
  Value associateFile(const Value& progId, const Value& description, const Value& extensions);
  Value associationExists(const Value& progId);

  // 退出事件循环并结束程序。返回 None。
  //
  // 参数收 Value 而不是 int，是为了与其它脚本门面一致：生成代码里所有
  // 表达式都是 py::Value，混入强类型参数会让脚本侧出现「有的函数要
  // 先转 int，有的不用」这种无法自解释的差异。
  Value quit(const Value& exitCode = Value());
  // 请求整窗重绘（脚本改了全局状态后调）。返回 None。
  Value refresh();

  // ── 外观 ──
  //
  // 脚本里写 set_theme("dark")。必须走窗口：控件是从视图树取当前主题的，
  // 只改一个字符串不会让任何东西重绘。
  //
  // 名字故意不和 THEME 常量撞：THEME 是构建期读进 AppSpec 的启动值，
  // set_theme 是运行期动作。两者同时存在，「记住上次选择」才写得出来。
  Value setTheme(const Value& name);
  Value themeName() const;
  // 改窗口标题。自绘标题栏与系统标题栏都会跟着变（见 AppWindow::setTitle）。
  Value setTitle(const Value& title);

 private:
  ViewGroup* root_ = nullptr;
  AppWindow* window_ = nullptr;
};

// 把脚本对象跑起来。流程固定，示例的 main.cpp 因此只有四行：
//
//   pyui::enableHighDpi();
//   Calculator app;
//   return pyui::py::run(app, argc, argv);
//
// 接线（脏区、焦点、光标闪烁、标题栏命中、拖放、关窗）全在 AppWindow 里，
// 这里只补脚本自己的那几条：键盘/字符兜底、帧定时器、托盘、关联文件。
int run(Script& script, int argc, char** argv);
// 同一个入口，参数从平台层取（Windows 上是 wWinMain 的 UTF-16 命令行）。
//
// 生成的入口用这一个：图形入口拿不到 char**，而「怎么把命令行变成 UTF-8
// 参数」属于平台细节，不该出现在每个应用的生成代码里。
int run(Script& script);
// 已有一份 UTF-8 参数表时直接用它（自写宿主的场景）。
int run(Script& script, const std::vector<std::string>& arguments);

// ── 控件方法 ──
//
// 名字与 Python 侧一致（set_text / text / progress …）。生成的代码把
// `find("a").set_text(x)` 翻成 `py::setText(find("a"), x)`，所以这里不需要
// 任何按字符串查表的动态分派。控件类型不对时是**静默无操作**——脚本里
// 「这个 id 上到底是哪个控件」写错时，界面不响应比崩溃好排查得多。
void setText(const Value& view, const Value& text);
Value text(const Value& view);
void setLabel(const Value& view, const Value& text);
Value label(const Value& view);
void setPlaceholder(const Value& view, const Value& text);
void setProgress(const Value& view, const Value& progress);
Value progress(const Value& view);
void setIndeterminate(const Value& view, const Value& busy);
void setChecked(const Value& view, const Value& checked);
Value checked(const Value& view);
void setValue(const Value& view, const Value& value);
Value value(const Value& view);
void setVisible(const Value& view, const Value& visible);
void setEnabled(const Value& view, const Value& enabled);
void setColor(const Value& view, const Value& color);
void setTextColor(const Value& view, const Value& color);
// 焦点：脚本用它在启动时把光标放进输入框
void focus(const Value& view);

// ── 字符串内建 ──
// 已迁到 py_value.h：它们是 Python 语言本身的方法，AiPy 不装 pyui 也要有。
// 这里不再声明，生成代码仍然 include py_script.h，所以来源对使用者透明。

// ── 事件绑定 ──
//
// 参数是 std::function 而不是 py::Value：生成代码在这里直接给出 lambda，
// 函数签名在编译期就对上。脚本里把 on_click 写成带参数的函数会在 pyuic
// 阶段报错，而不是运行期某个回调静默不触发。
void onClick(const Value& view, std::function<void()> fn);
void onDoubleClick(const Value& view, std::function<void()> fn);
// 值变化：滑块给出数值，输入框给出文本，复选给出 True/False
void onChange(const Value& view, std::function<void(const Value&)> fn);
void onCommit(const Value& view, std::function<void(const Value&)> fn);
void onSubmit(const Value& view, std::function<void(const Value&)> fn);
// 右键：脚本自己决定弹什么
void onContextMenu(const Value& view, std::function<void()> fn);

// ── 菜单与类型辅助 ──
//
// 脚本侧无法持有 MenuItem（它是库里的结构体，而 Value 只有基础类型），
// 所以菜单在脚本里的表示是一条约定：
//   menu_item(id, label) → "id|label"
//   menu_separator()     → None
// 这样列表字面量就能直接写
//   [menu_item("new", "新建"), menu_separator(), menu_item("quit", "退出")]
// 而不必为了菜单再造一个 Value 种类。
Value makeMenuItem(const Value& id, const Value& label);
Value makeMenuSeparator();
// 把上面那种编码解回 MenuItem 数组；后端与 TrayHost 只认后者。
std::vector<MenuItem> menuItemsFrom(const Value& items);

// `parseNumber` / `charFromCode` 不在这里声明：它们是**语言内建**
// （脚本里的 parse_number / char_from_code），实现归 AiPy。
// py_value.h 的 `using namespace aipy` 已经把名字带进 pyui::py，
// 谁再在这里声明一份，就会是「有声明、无定义」的链接错误。

// 键位 → 脚本里用的名字："0".."9"、"a".."z"、"Enter"、"Escape"、"+" 等。
// 用字符串而不是枚举，是因为脚本语言里没有枚举，而 `if key == "Enter"` 是最
// 自然的写法。
std::string keyName(const Key& key);
Value modifiersValue(const Modifiers& mods);

}  // namespace py
}  // namespace pyui
