# AiUI 组件文档

自绘 UI 库的使用说明。所有控件都不创建系统子控件，像素由系统绘图库绘制；
`include/` 下的公开头文件不出现 `HWND` / `HDC` / `COLORREF` 等平台类型，
平台差异全部收在 `src/os/<platform>/` 里（Windows 已实现，macOS / Linux 预留）。

## 设计约定

**数据驱动，不写子类。** 控件自己持有数据、自己绘制。调用方只做两件事：
配好样式、传进数据，再挂几个回调。`AiZip` 里没有任何 `View` 子类。

**尺寸按 96 DPI 书写。** 所有 `int` 尺寸参数都是设计值，控件内部用
`dpi() / 96` 换算成物理像素。写 `44` 就是 100% 缩放下的 44px，在 150%
缩放下自动变成 66px。

**局部重绘。** 悬停只重画进出两行，选中只重画行区，滚动只重画内容区。
控件不整体刷新，更不整窗刷新。

## 目录

### 渲染核心

| 类 | 职责 |
| --- | --- |
| `View` | 一块矩形区域：测量、绘制、事件、局部刷新 |
| `ViewGroup` | 管子树的增删、摆放、命中与逐层绘制 |
| `ViewRoot` | 整棵树唯一的窗口，按系统更新区重绘，统一管焦点 / 捕获 / 光标 |
| `ScrollView` | 可滚动视图基类，自绘滚动条并处理滚轮与滑块拖动 |

整棵界面树共用一个原生窗口：子视图没有任何句柄，`WM_PAINT` 只由
`ViewRoot` 收一次，再按脏区递归绘制。拖动窗口、超出屏幕、跨显示器
都不会留下空白。

### 布局

- [FrameLayout](layouts/frame_layout.md) —— 绝对坐标叠放
- [LinearLayout](layouts/linear_layout.md) —— 线性排列 + weight 分配

### 组件

| 组件 | 用途 |
| --- | --- |
| [Button](widgets/button.md) | 圆角描边按钮，图标 + 文字 |
| [ButtonBar](widgets/button_bar.md) | 横向按钮条，支持分组分隔 |
| [BreadcrumbBar](widgets/breadcrumb_bar.md) | 后退 + 路径节点 + 搜索框 |
| [InputBox](widgets/input_box.md) | 圆角输入框，含选区与 IME |
| [ListView](widgets/list_view.md) | 虚拟列表，适配器按需供数 |
| [TreeView](widgets/tree_view.md) | 懒加载树，扁平行渲染 |
| [StatusBar](widgets/status_bar.md) | 路径 + 格式标签 + 统计 |
| [TextView](widgets/text_view.md) | 单行文字，对齐与省略 |
| [ImageView](widgets/image_view.md) | 位图或矢量图标 |

## 起步

### 1. 建控件树

```cpp
#include "AiUI/layouts/linear_layout.h"
#include "AiUI/widgets/list_view.h"

auto* root = new LinearLayout(Orientation::kVertical);
auto* list = root->add<ListView>();
root->addView(list, LayoutParams{}.withWeight(1.0f));
```

`addView()` 返回子视图指针，所有权归父容器，父容器析构时递归释放。
换父容器必须先 `removeView()`（它会销毁对象），不能直接把视图挪走。

### 2. 挂到宿主窗口

```cpp
// 根视图自己创建并持有一个 ViewRoot，整棵树只此一个窗口
root->create(hwnd, AiUI::Rect{});
// 窗口尺寸变化时重新排布
root->layout(AiUI::Rect{0, 0, width, height});
```

`create()` 建出唯一的原生窗口并递归挂载子树，比 `addView()` 晚也没关系，
之后加入的子视图会立即挂载。

### 3. 用 XML 描述布局（可选）

标签名直接就是控件类名，构建期转成 C++ 并加入编译：

```xml
<LinearLayout orientation="vertical">
  <BreadcrumbBar id="crumb" height="48"/>
  <ButtonBar id="toolBar" height="52"/>
  <LinearLayout id="content" orientation="horizontal" weight="1">
    <TreeView id="tree" width="230" height="match"/>
    <ListView id="list" weight="1" height="match"/>
  </LinearLayout>
  <StatusBar id="status" height="36"/>
</LinearLayout>
```

```cmake
aiui_add_layouts(AiZip assets/layouts/main_frame.xml)
```

生成 `AiUI::generated::MainFrame`，带 `id` 的节点成为公开成员指针，
代码里直接 `frame.xxx` 取用；也可以用 `find<T>()` 按类型找后代。
XML 只描述"放在哪、多大"，行为在代码里配置。

## 通用参数

`LayoutParams` 描述子视图在容器里的摆放方式，所有控件通用。

```cpp
LayoutParams{}
    .withSize(200, 44)   // 宽高：kWrapContent / kMatchParent / 设计值
    .margin(8, 4)        // 外边距
    .offset(12, 12)      // 绝对偏移，FrameLayout 用
    .withWeight(1.0f)    // LinearLayout 里按比例吃剩余空间
    .align(Gravity::kCenter, Gravity::kCenter);
```

| 字段 | 说明 |
| --- | --- |
| `width` / `height` | `-1`(`kWrapContent`) 按内容，`-2`(`kMatchParent`) 撑满，其余为设计值 |
| `weight` | 大于 0 时参与 `LinearLayout` 剩余空间分配 |
| `margin*` | 四边外边距；只有 `margin()` 系列便捷方法，没有 `padding` |
| `x` / `y` | `FrameLayout` 用的绝对偏移 |

## 控件基类能力

任何控件都能用，不用关心具体类型：

| 方法 | 说明 |
| --- | --- |
| `show(bool)` | 显示或隐藏；隐藏后不参与绘制 |
| `setEnabled(bool)` | 禁用状态，控件自己画灰 |
| `setFocus()` | 请求键盘焦点 |
| `invalidate()` | 整控件重绘 |
| `invalidateRect(rect)` | 只重绘一块区域（局部坐标） |
| `bounds()` | 相对父容器的矩形 |
| `clientRect()` | 自身局部矩形，恒为 `{0,0,w,h}` |
| `find<T>()` | 深度优先找第一个匹配类型的后代 |
| `handle()` | 根视图（`ViewRoot`）的窗口句柄，用于宿主挂拖放等 |

## 坐标约定

- `bounds()` 相对父容器；`clientRect()` 是自己内部的 `{0,0,w,h}`
- 控件内部一律用自身局部坐标，平移由 `ViewGroup` 逐层 `painter.translate()`
- `invalidateRect()` 收局部坐标，`ViewRoot::invalidateRect()` 收窗口坐标

## 需要完全自定义绘制时

数据驱动覆盖不了时才派生，只需覆盖 `onPaint`：

```cpp
class MyGauge : public AiUI::View {
 protected:
  void onPaint(AiUI::Painter& painter, const AiUI::Rect& rect) override {
    painter.fillRoundRect(rect, AiUI::kColorPane, rect.height() / 2);
  }
};
```

需要装子视图就派生 `ViewGroup` 并覆盖 `arrangeChildren()`，需要滚动就派生
`ScrollView` 并实现 `contentHeight()`。

可用图元见 `AiUI/painter.h`：填充、圆角、描边、渐变、文字、图标、位图、
裁剪、平移。
