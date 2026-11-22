#include "pyui/py_script.h"

#include "pyui/app_window.h"
#include "pyui/layout_inflater.h"
#include "pyui/painter.h"
#include "pyui/platform/platform.h"
#include "pyui/theme.h"
#include "pyui/view_group.h"
#include "pyui/widgets/button.h"
#include "pyui/widgets/check_box.h"
#include "pyui/widgets/progress_bar.h"
#include "pyui/widgets/radio_button.h"
#include "pyui/widgets/slider.h"
#include "pyui/widgets/text_field.h"
#include "pyui/widgets/text_view.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#ifdef PYUI_OS_WINDOWS
#include <windows.h>
#endif

namespace pyui {
namespace py {

// ── 控件定位 ──

Value Script::find(const Value& id) const {
  if (root_ == nullptr) return Value();
  // 脚本里更常写 find("total") 而不是 find(id)，所以这里接受任意值：
  // 非字符串按 toString 再查一次，写错类型也能给出结果而不是静默失败。
  const std::string name = id.toString();
  if (name.empty()) return Value();
  View* view = root_->findView(name.c_str());
  if (view == nullptr) return Value();
  return Value(view);
}

// ── 平台子系统（脚本门面）──

Value Script::trayShow(const Value& tooltip) {
  TrayHost* tray = platform().tray();
  if (tray == nullptr) return Value(false);

  const std::string text = tooltip.toString();
  tray->setOnActivate([this]() {
    // 托盘左键：把窗口拉回前台。最小化时直接 setFocus 没用，先还原。
    if (window_ != nullptr && window_->getWindow() != nullptr) {
      window_->getWindow()->setFocus();
    }
    onTrayActivate();
  });
  tray->setMenu([this]() { return trayMenu(); },
                [this](const std::string& id) { onTrayCommand(id); });
  return Value(tray->show(text));
}

Value Script::trayHide() {
  if (TrayHost* tray = platform().tray()) tray->hide();
  return Value();
}

Value Script::traySetTooltip(const Value& tooltip) {
  if (TrayHost* tray = platform().tray()) tray->setTooltip(tooltip.toString());
  return Value();
}

Value Script::menuPopup(const Value& items) {
  ContextMenuHost* menu = platform().contextMenu();
  const std::vector<MenuItem> decoded = menuItemsFrom(items);
  if (menu == nullptr || decoded.empty()) return Value(std::string());
  if (window_ == nullptr || window_->getWindow() == nullptr) return Value(std::string());

  PopupMenuRequest request;
  request.owner = window_->getWindow()->handle();
  // 不提前台，点菜单外面菜单不会消失——这是 Win32 弹出菜单最经典的坑
  menu->bringToFront(request.owner);
  // 弹在鼠标当前位置而不是窗口坐标：托盘与右键菜单都不是「相对某个控件」
  request.screenPosition = menu->cursorPosition();
  request.items = decoded;
  return Value(menu->showPopupMenu(request));
}

Value Script::associateFile(const Value& progId, const Value& description,
                            const Value& extensions) {
  FileAssociationHost* host = platform().fileAssociations();
  if (host == nullptr) return Value("当前平台不支持文件关联");

  FileAssociation association;
  association.progId = progId.toString();
  association.description = description.toString();
  for (const Value& extension : iterate(extensions)) {
    const std::string text = extension.toString();
    // 后端要求后缀带点；脚本里写 "pynote" 比 ".pynote" 自然，这里补上
    association.extensions.push_back(text.empty() || text.front() == '.' ? text : "." + text);
  }
  const std::string error = host->registerFileAssociations(association);
  if (error.empty()) return Value();
  return Value(error);
}

Value Script::associationExists(const Value& progId) {
  FileAssociationHost* host = platform().fileAssociations();
  if (host == nullptr) return Value(false);
  FileAssociation association;
  association.progId = progId.toString();
  return Value(host->areFileAssociationsRegistered(association));
}

Value Script::quit(const Value& exitCode) {
  platform().quitEventLoop(static_cast<int>(exitCode.toInt()));
  return Value();
}

Value Script::refresh() {
  if (window_ != nullptr) window_->refreshAll();
  return Value();
}

// ── 外观 ──

Value Script::setTheme(const Value& name) {
  if (window_ == nullptr) return Value("窗口还没建出来");
  // themeByName 遇到不认识的名字会退回浅色，这里不额外校验：
  // 脚本里把名字写错时看到的是「变回浅色」，比弹一个错误更有用。
  window_->setTheme(themeByName(name.toString().c_str()));
  return Value();
}

Value Script::themeName() const {
  if (window_ == nullptr || window_->theme() == nullptr) return Value("light");
  const char* name = window_->theme()->name;
  return Value(std::string(name != nullptr ? name : "light"));
}

Value Script::setTitle(const Value& title) {
  if (window_ != nullptr) window_->setTitle(title.toString());
  return Value();
}

// ── 启动 ──

namespace {

// 关联文件双击打开：命令行里第一个不像选项的参数。
// Windows 把关联文件交给 shell\open\command 时是 "<exe>" "%1"，所以它总在
// 第一个位置；跳过 - 开头的参数是为了不把 --debug 当成文件名。
std::string fileArgument(const std::vector<std::string>& arguments) {
  for (size_t i = 1; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    if (argument.empty()) continue;
    if (argument[0] == '-') continue;
    return argument;
  }
  return std::string();
}

}  // namespace

int run(Script& script) {
  // 参数从平台层取：Windows 的图形入口是 wWinMain，没有 char**。
  return run(script, platform().commandLineArguments());
}

int run(Script& script, int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(argc > 0 ? static_cast<size_t>(argc) : 0);
  for (int i = 0; i < argc; ++i) {
    if (argv[i] != nullptr) arguments.emplace_back(argv[i]);
  }
  return run(script, arguments);
}

int run(Script& script, const std::vector<std::string>& arguments) {
  AppSpec spec;
  script.describe(spec);

  AppWindow::Options options;
  options.title = spec.title;
  options.width = spec.width > 0 ? spec.width : 640;
  options.height = spec.height > 0 ? spec.height : 480;
  options.minWidth = spec.minWidth;
  options.minHeight = spec.minHeight;
  options.customTitleBar = spec.customTitleBar;
  options.resizable = spec.resizable;

  AppWindow window(options);
  script.setWindow(&window);
  window.setTheme(themeByName(spec.theme.c_str()));

  // 布局名来自脚本的 LAYOUT 常量。缺失时把「哪一步错了」说清楚：
  // 这是唯一一处业务代码写错就立刻停下来的地方，信息量要给足。
  if (spec.layout.empty()) {
    std::fprintf(stderr, "pyui: 脚本没有设置 LAYOUT，无法加载界面\n");
    return 2;
  }
  auto content = LayoutInflater::inflate(spec.layout);
  if (content == nullptr) {
    std::fprintf(stderr, "pyui: 找不到布局 \"%s\"（XML 文件名去掉扩展名）\n",
                 spec.layout.c_str());
    return 2;
  }

  // 先挂 root 再 build：脚本里的 find("x") 要靠 root 才能找到控件。
  script.setRoot(content.get());
  window.setContentView(std::move(content));

  // 顺序不能换：模块级语句（变量初值）先跑，再轮到脚本的 def build()。
  //
  // 对应 Python 的语义：模块体在 import 时就执行完了，而 build() 是一个
  // 钩子。把初值放进 build() 会让「变量初值」依赖窗口已经存在，
  // 那时 find() 在初始化阶段就有了意义——那是错的。
  script.initState();
  script.build();

  if (spec.frameInterval > 0) window.setFrameTimer(spec.frameInterval);
  if (!spec.trayTooltip.empty()) (void)script.trayShow(Value(spec.trayTooltip));

  // 视图树没消费的输入才交给脚本。顺序不能反：脚本先吃键会让输入框
  // 永远收不到 Backspace 和方向键。
  window.setKeyHandler([&script](const Key& key, const Modifiers& mods) {
    return script.onKeyDown(key, mods);
  });
  window.setCharHandler([&script](std::uint32_t codepoint) { return script.onChar(codepoint); });
  window.setOnClose([&script]() { return script.onCloseRequest(); });
  window.setOnFileDrop(
      [&script](const std::vector<std::string>& paths) { script.onFileDrop(paths); });

  // 关联登记是纯副作用，失败不该阻止程序启动：现代 Windows 不允许静默抢占
  // 默认程序，登记成功也只做到「出现在打开方式里」，因此报错提示反而是噪音。
  if (!spec.progId.empty()) {
    (void)script.associateFile(Value(spec.progId), Value(spec.fileDescription),
                               Value::list(Value::List(spec.extensions.begin(),
                                                       spec.extensions.end())));
  }

  // 关联文件在窗口显示后再打开：openFile 里通常要改标题、填内容，
  // 而这些操作都要求控件已经挂上根。
  const std::string opened = fileArgument(arguments);
  if (!opened.empty() && window.create()) script.openFile(opened);

  return window.run();
}

// ═══════════════════════════════════════════════════════════════
// 控件方法
// ═══════════════════════════════════════════════════════════════

namespace {

// 取控件；类型不对返回 nullptr。全部访问器都走它，于是「id 上挂错了控件类型」
// 这件事只在这里判断一次，而不是每个函数各写一遍 dynamic_cast。
template <typename T>
T* asView(const Value& view) {
  return dynamic_cast<T*>(view.asView());
}

View* asAny(const Value& view) { return view.asView(); }

}  // namespace

void setText(const Value& view, const Value& text) {
  if (View* target = asAny(view)) {
    // TextField 与 TextView 都有「文本」这个概念，但它们的 setter 不同名：
    // 脚本只写 set_text，落到哪个 setter 由控件类型决定，这正是「脚本不该
    // 感知控件类名」的体现。
    if (auto* field = dynamic_cast<TextField*>(target)) {
      field->setText(text.toString());
      return;
    }
    if (auto* label = dynamic_cast<TextView*>(target)) {
      label->setText(text.toString());
    }
  }
}

Value text(const Value& view) {
  if (auto* field = asView<TextField>(view)) return Value(field->text());
  if (auto* label = asView<TextView>(view)) return Value(label->text());
  return Value(std::string());
}

void setLabel(const Value& view, const Value& text) {
  if (auto* button = asView<Button>(view)) {
    button->setLabel(text.toString());
    return;
  }
  if (auto* box = asView<CheckBox>(view)) {
    box->setLabel(text.toString());
    return;
  }
  if (auto* radio = asView<RadioButton>(view)) {
    radio->setLabel(text.toString());
    return;
  }
  // ProgressBar 的 label 是「条上文字」，与按钮文字语义接近，一并接受
  if (auto* bar = asView<ProgressBar>(view)) bar->setLabel(text.toString());
}

Value label(const Value& view) {
  if (auto* button = asView<Button>(view)) return Value(button->label());
  if (auto* box = asView<CheckBox>(view)) return Value(box->label());
  if (auto* radio = asView<RadioButton>(view)) return Value(radio->label());
  if (auto* bar = asView<ProgressBar>(view)) return Value(bar->label());
  return Value(std::string());
}

void setPlaceholder(const Value& view, const Value& text) {
  if (auto* field = asView<TextField>(view)) field->setPlaceholder(text.toString());
}

void setProgress(const Value& view, const Value& progress) {
  if (auto* bar = asView<ProgressBar>(view)) {
    bar->setProgress(static_cast<float>(progress.toFloat()));
    return;
  }
  // 滑块也接受 set_progress：脚本里「把进度条推到某个值」与「把滑块推到
  // 某个值」是同一件事，没必要逼用户记两个名字。
  if (auto* slider = asView<Slider>(view)) slider->setValue(slider->minimum() +
                                                           progress.toFloat() *
                                                               (slider->maximum() -
                                                                slider->minimum()));
}

Value progress(const Value& view) {
  if (auto* bar = asView<ProgressBar>(view)) return Value(static_cast<double>(bar->progress()));
  if (auto* slider = asView<Slider>(view)) {
    const double span = slider->maximum() - slider->minimum();
    if (span <= 0.0) return Value(0.0);
    return Value((slider->value() - slider->minimum()) / span);
  }
  return Value(0.0);
}

void setIndeterminate(const Value& view, const Value& busy) {
  if (auto* bar = asView<ProgressBar>(view)) bar->setIndeterminate(busy.toBool());
}

void setChecked(const Value& view, const Value& checked) {
  if (auto* box = asView<CheckBox>(view)) {
    box->setChecked(checked.toBool());
    return;
  }
  if (auto* radio = asView<RadioButton>(view)) radio->setChecked(checked.toBool());
}

Value checked(const Value& view) {
  if (auto* box = asView<CheckBox>(view)) return Value(box->isChecked());
  if (auto* radio = asView<RadioButton>(view)) return Value(radio->isChecked());
  return Value(false);
}

void setValue(const Value& view, const Value& value) {
  if (auto* slider = asView<Slider>(view)) {
    slider->setValue(value.toFloat());
    return;
  }
  // 输入框也接受 set_value：脚本里「把值放进框里」与「把值交给滑块」
  // 只差一个目标类型，用同一个名字更顺。
  if (auto* field = asView<TextField>(view)) field->setText(value.toString());
}

Value value(const Value& view) {
  if (auto* slider = asView<Slider>(view)) return Value(slider->value());
  if (auto* field = asView<TextField>(view)) return Value(field->text());
  return Value();
}

void setVisible(const Value& view, const Value& visible) {
  if (View* target = asAny(view)) target->setVisible(visible.toBool());
}

void setEnabled(const Value& view, const Value& enabled) {
  if (View* target = asAny(view)) target->setEnabled(enabled.toBool());
}

void setColor(const Value& view, const Value& color) {
  if (View* target = asAny(view)) target->setBackgroundColor(static_cast<Color>(color.toInt()));
}

void setTextColor(const Value& view, const Value& color) {
  // 只有 TextView 有「文字色」这个概念；输入框的墨色跟随主题。
  // 脚本里 set_text_color 通常用在标题与状态行上，落到 TextView 就够。
  if (auto* label = asView<TextView>(view)) {
    label->setColor(static_cast<Color>(color.toInt()));
  }
}

void focus(const Value& view) {
  if (View* target = asAny(view)) target->setFocus();
}

// ── 字符串方法 ──
//
// 列表方法（append/extend/pop …）在 py_value.cpp 里：它们是容器语义，
// 与「脚本 API」无关，生成代码对它们不做名字改写。

Value upper(const Value& text) {
  std::string out = text.toString();
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  }
  return Value(std::move(out));
}

Value lower(const Value& text) {
  std::string out = text.toString();
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return Value(std::move(out));
}

Value strip(const Value& text) {
  const std::string& source = text.toString();
  size_t begin = 0;
  size_t end = source.size();
  auto space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (begin < end && space(static_cast<unsigned char>(source[begin]))) ++begin;
  while (end > begin && space(static_cast<unsigned char>(source[end - 1]))) --end;
  return Value(source.substr(begin, end - begin));
}

Value replace(const Value& text, const Value& from, const Value& to) {
  std::string out = text.toString();
  const std::string needle = from.toString();
  if (needle.empty()) return Value(std::move(out));
  const std::string replacement = to.toString();
  size_t position = 0;
  while ((position = out.find(needle, position)) != std::string::npos) {
    out.replace(position, needle.size(), replacement);
    position += replacement.size();
  }
  return Value(std::move(out));
}

Value split(const Value& text, const Value& separator) {
  const std::string source = text.toString();
  const std::string needle = separator.toString();
  Value::List parts;
  if (needle.empty()) {
    // 不传分隔符时按空白切：这是 Python 的 split() 无参语义，
    // 也是「从一行词里取第一个」最常见的用法。
    std::string current;
    for (char c : source) {
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        if (!current.empty()) {
          parts.emplace_back(std::move(current));
          current.clear();
        }
      } else {
        current.push_back(c);
      }
    }
    if (!current.empty()) parts.emplace_back(std::move(current));
    return Value::list(std::move(parts));
  }

  size_t start = 0;
  for (;;) {
    const size_t hit = source.find(needle, start);
    if (hit == std::string::npos) {
      parts.emplace_back(source.substr(start));
      break;
    }
    parts.emplace_back(source.substr(start, hit - start));
    start = hit + needle.size();
  }
  return Value::list(std::move(parts));
}

Value join(const Value& separator, const Value& items) {
  const std::string glue = separator.toString();
  std::string out;
  bool first = true;
  for (const Value& item : iterate(items)) {
    if (!first) out += glue;
    // 非字符串元素按 str() 处理：`" ".join([1, 2])` 在 Python 里会抛错，
    // 但脚本里更常见的意图是「拼成文本」，报错在这里没有收益。
    out += item.toString();
    first = false;
  }
  return Value(std::move(out));
}

Value startsWith(const Value& text, const Value& prefix) {
  const std::string source = text.toString();
  const std::string needle = prefix.toString();
  return Value(source.size() >= needle.size() && source.compare(0, needle.size(), needle) == 0);
}

Value endsWith(const Value& text, const Value& suffix) {
  const std::string source = text.toString();
  const std::string needle = suffix.toString();
  return Value(source.size() >= needle.size() &&
               source.compare(source.size() - needle.size(), needle.size(), needle) == 0);
}

Value findIn(const Value& text, const Value& needle) {
  const std::string& source = text.toString();
  const std::string target = needle.toString();
  const size_t hit = source.find(target);
  if (hit == std::string::npos) return Value(-1LL);
  // 返回**字符**下标而不是字节下标：脚本拿去切片时两者必须一致，
  // 中文文本里混用会出现「找到的位置切出来是半个字」。
  return Value(static_cast<long long>(utf8Length(std::string_view(source).substr(0, hit))));
}

Value countIn(const Value& text, const Value& needle) {
  const std::string& source = text.toString();
  const std::string target = needle.toString();
  if (target.empty()) return Value(0LL);
  long long count = 0;
  size_t position = 0;
  while ((position = source.find(target, position)) != std::string::npos) {
    ++count;
    position += target.size();
  }
  return Value(count);
}

Value textOfChar(const Value& codepoint) {
  return Value(utf8Encode(static_cast<std::uint32_t>(codepoint.toInt())));
}

// str.format() 的最小实现：只支持 {} 与 {0} {1} 这类按位置替换。
// 不做格式规格（{:.2f}）——那要一整套格式迷你语言，而脚本里
// 「先 round 再 str」已经够用，硬塞半个实现反而会让人以为支持全了。
Value formatText(const Value& text, const Value& args) {
  const std::string& pattern = text.toString();
  const std::vector<Value> values = iterate(args);
  std::string out;
  size_t index = 0;
  size_t next = 0;
  while (index < pattern.size()) {
    const size_t open = pattern.find('{', index);
    if (open == std::string::npos) {
      out += pattern.substr(index);
      break;
    }
    const size_t close = pattern.find('}', open + 1);
    if (close == std::string::npos) {
      // 没有配对的 }：原样保留剩下的部分，不要吞掉用户文本
      out += pattern.substr(index);
      break;
    }
    out += pattern.substr(index, open - index);
    const std::string spec = pattern.substr(open + 1, close - open - 1);
    if (spec.empty()) {
      if (next < values.size()) out += values[next++].toString();
    } else {
      const auto explicitIndex = static_cast<size_t>(std::strtoull(spec.c_str(), nullptr, 10));
      if (explicitIndex < values.size()) out += values[explicitIndex].toString();
    }
    index = close + 1;
  }
  return Value(std::move(out));
}

Value minimum(const Value& a, const Value& b) { return lt(a, b).toBool() ? a : b; }
Value maximum(const Value& a, const Value& b) { return gt(a, b).toBool() ? a : b; }

void log(const Value& value) {
  // Windows 上 stderr 在 GUI 程序里没有窗口可写，OutputDebugString 才是
  // 能被调试器看到的那条路。用 platform 的能力判断太绕，直接看宏。
#ifdef PYUI_OS_WINDOWS
  std::string text = value.toString();
  text += "\n";
  ::OutputDebugStringA(text.c_str());
#else
  std::fprintf(stderr, "[pyui] %s\n", value.toString().c_str());
#endif
}

// ── 文件访问 ──
//
// 用 std::FILE 而不是 iostream：要的只是「按字节读进来 / 按字节写出去」，
// 流对象带来的区域设置与格式化能力在这里全是负担。

namespace {

// 路径分隔符跟随平台。脚本里拼路径必须走 joinPath，否则在另一个平台上
// "\" 会被当成文件名的一部分。
char separatorChar() { return platform().pathSeparator(); }

}  // namespace

Value readFile(const Value& path) {
  const std::string name = path.toString();
  if (name.empty()) return Value();

  std::FILE* file = std::fopen(name.c_str(), "rb");
  if (file == nullptr) return Value();

  std::string content;
  char buffer[4096];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) content.append(buffer, got);
  std::fclose(file);

  // 去掉 UTF-8 BOM：Windows 记事本保存的文本带它，留在正文里会让
  // 标题的第一个字显示成乱码方块。
  if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
      static_cast<unsigned char>(content[1]) == 0xBB &&
      static_cast<unsigned char>(content[2]) == 0xBF) {
    content.erase(0, 3);
  }
  return Value(std::move(content));
}

Value writeFile(const Value& path, const Value& text) {
  const std::string name = path.toString();
  if (name.empty()) return Value("路径为空");

  std::FILE* file = std::fopen(name.c_str(), "wb");
  if (file == nullptr) return Value("无法写入 " + name);

  const std::string content = text.toString();
  const size_t written = std::fwrite(content.data(), 1, content.size(), file);
  std::fclose(file);
  // 短写说明磁盘满了或配额用尽。必须报出来：静默截断一个笔记文件，
  // 用户会以为保存成功。
  if (written != content.size()) return Value("只写出 " + std::to_string(written) + " / " +
                                              std::to_string(content.size()) + " 字节");
  return Value();
}

Value joinPath(const Value& left, const Value& right) {
  std::string head = left.toString();
  const std::string tail = right.toString();
  if (head.empty()) return Value(tail);
  if (tail.empty()) return Value(head);
  const char sep = separatorChar();
  if (head.back() != sep && head.back() != '/' && head.back() != '\\') head.push_back(sep);
  return Value(std::move(head) + tail);
}

Value homeDirectory() {
  const std::string home = platform().homeDirectory();
  if (home.empty()) return Value();
  return Value(home);
}

// ── 事件绑定 ──

void onClick(const Value& view, std::function<void()> fn) {
  if (View* target = asAny(view)) target->setOnClick(std::move(fn));
}

void onDoubleClick(const Value& view, std::function<void()> fn) {
  if (View* target = asAny(view)) {
    target->setOnDoubleClick([fn](const MouseEvent&) { fn(); });
  }
}

void onChange(const Value& view, std::function<void(const Value&)> fn) {
  if (View* target = asAny(view)) {
    if (auto* slider = dynamic_cast<Slider*>(target)) {
      // 滑块回调给的是 double；包一层转成 Value，脚本侧统一看到数字
      slider->setOnChange([fn](double value) { fn(Value(value)); });
      return;
    }
    if (auto* field = dynamic_cast<TextField*>(target)) {
      field->setOnChange([fn](const std::string& text) { fn(Value(text)); });
      return;
    }
    if (auto* box = dynamic_cast<CheckBox*>(target)) {
      box->setOnToggle([fn](CheckState state) {
        fn(Value(state == CheckState::kChecked));
      });
      return;
    }
    if (auto* radio = dynamic_cast<RadioButton*>(target)) {
      // RadioButton 自身只有 onClick；变化回调挂在它的组上（见示例）
      radio->setOnClick([fn]() { fn(Value(true)); });
    }
  }
}

void onCommit(const Value& view, std::function<void(const Value&)> fn) {
  if (auto* slider = asView<Slider>(view)) {
    // 与 onChange 的区别只在触发时机：拖动过程不回调，松手才给一次。
    // 「拖动时实时刷新、松手再保存」这类界面必须能分开。
    slider->setOnCommit([fn](double value) { fn(Value(value)); });
    return;
  }
  onChange(view, std::move(fn));
}

void onSubmit(const Value& view, std::function<void(const Value&)> fn) {
  if (auto* field = asView<TextField>(view)) {
    // 返回 true 表示已消费回车，否则回车会继续冒泡给宿主快捷键
    field->setOnSubmit([fn](const std::string& text) {
      fn(Value(text));
      return true;
    });
  }
}

void onContextMenu(const Value& view, std::function<void()> fn) {
  if (View* target = asAny(view)) {
    target->setOnContextMenu([fn](const MouseEvent&) { fn(); });
  }
}

// ── 菜单 ──
//
// 脚本侧无法持有 MenuItem（它是库里的结构体，而 Value 只有基础类型），
// 所以菜单在脚本里的表示是一条约定：menu_item(id, label) 生成 "id|label"，
// menu_separator() 生成 None。这样列表字面量就能直接写
//   [menu_item("new", "新建"), menu_separator(), menu_item("quit", "退出")]
// 而不必为菜单再造一个 Value 种类。

Value makeMenuItem(const Value& id, const Value& label) {
  // Value 承载不了 MenuItem（它是库里的结构体），所以编码成 "id|label"。
  // 分隔符选 | 是因为菜单文案里几乎不会出现它，而 id 本来就是标识符。
  return Value(id.toString() + "|" + label.toString());
}

Value makeMenuSeparator() { return Value(); }

std::vector<MenuItem> menuItemsFrom(const Value& items) {
  std::vector<MenuItem> out;
  for (const Value& item : items.asList()) {
    if (item.isNone()) {
      out.push_back(MenuItem::separatorItem());
      continue;
    }
    const std::string encoded = item.toString();
    const size_t bar = encoded.find('|');
    if (bar == std::string::npos) {
      // 直接给字符串时按「id 与文案相同」处理：手写菜单的常见简写
      out.push_back(MenuItem::action(encoded, encoded));
      continue;
    }
    out.push_back(MenuItem::action(encoded.substr(0, bar), encoded.substr(bar + 1)));
  }
  return out;
}

// ── 类型辅助 ──

Value parseNumber(const Value& text) {
  const std::string& source = text.toString();
  if (source.empty()) return Value();

  // 只认「整串都是数字」：输入框里 "12abc" 应该报错而不是变成 12。
  // strtod 会停在 a 处并返回 12，所以必须再检查剩余部分。
  const char* begin = source.c_str();
  char* end = nullptr;
  const double value = std::strtod(begin, &end);
  if (end == begin) return Value();
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return Value();
  return Value(value);
}

Value charFromCode(const Value& codepoint) {
  return Value(utf8Encode(static_cast<std::uint32_t>(codepoint.toInt())));
}

std::string keyName(const Key& key) {
  switch (key) {
    case Key::kNone:
      return std::string();
    case Key::kBackspace:
      return "Backspace";
    case Key::kTab:
      return "Tab";
    case Key::kEnter:
      return "Enter";
    case Key::kEscape:
      return "Escape";
    case Key::kSpace:
      return "Space";
    case Key::kInsert:
      return "Insert";
    case Key::kDelete:
      return "Delete";
    case Key::kHome:
      return "Home";
    case Key::kEnd:
      return "End";
    case Key::kPageUp:
      return "PageUp";
    case Key::kPageDown:
      return "PageDown";
    case Key::kLeft:
      return "Left";
    case Key::kUp:
      return "Up";
    case Key::kRight:
      return "Right";
    case Key::kDown:
      return "Down";
    default:
      break;
  }
  // 字母与数字在枚举里是连续区间，直接算回来
  const int index = static_cast<int>(key);
  if (index >= static_cast<int>(Key::kA) && index <= static_cast<int>(Key::kZ)) {
    return std::string(1, static_cast<char>('a' + (index - static_cast<int>(Key::kA))));
  }
  if (index >= static_cast<int>(Key::k0) && index <= static_cast<int>(Key::k9)) {
    return std::string(1, static_cast<char>('0' + (index - static_cast<int>(Key::k0))));
  }
  if (index >= static_cast<int>(Key::kF1) && index <= static_cast<int>(Key::kF12)) {
    return "F" + std::to_string(index - static_cast<int>(Key::kF1) + 1);
  }
  return std::string();
}

Value modifiersValue(const Modifiers& mods) {
  // 返回字符串而不是字典：脚本里最常见的写法是 `if "ctrl" in mods`，
  // 而 contains 对字符串就是子串查找，天然成立。
  std::string out;
  if (mods.ctrl) out += "ctrl+";
  if (mods.shift) out += "shift+";
  if (mods.alt) out += "alt+";
  if (mods.meta) out += "meta+";
  return Value(std::move(out));
}

}  // namespace py
}  // namespace pyui
