# AiUI

自绘 UI 库。控件不创建系统子控件，所有像素都由系统绘图库画出来，平台相关代码
收在 `src/os/<platform>/`，公开头文件不出现 `HWND` / `HDC` / `COLORREF`。

## 架构

```
include/AiUI/
  types.h        基础类型：Rect / Point / Size / Color / Gravity
  events.h       中性事件：MouseEvent / Key / Modifiers / CursorShape
  platform.h     平台后端接口（窗口、字体、图片、绘制子系统）
  painter.h      PaintBackend（最小图元）+ Painter（组合绘制）+ Canvas（双缓冲）
  theme.h        配色与尺寸常量（96 DPI 设计值）
  icon.h         矢量图标 ID
  font.h         FontSet：按 DPI 创建的一组界面字体
  view.h         View：绘制与事件的基本单位
  view_group.h   ViewGroup：可容纳子视图的容器
  view_root.h    ViewRoot：整棵树唯一的窗口与绘制管理者
  scroll_view.h  ScrollView：可滚动视图基类，滚动条自绘
  window.h       Window：标题栏 + 内容区的窗口组件（模态/非模态）
  layout_inflater.h 按 XML 文件名取构建期生成的布局类
  layouts/       FrameLayout / LinearLayout
  widgets/       Button / ButtonBar / BreadcrumbBar / InputBox /
                 ListView / TreeView / StatusBar / TextView / ImageView /
                 TitleBar
src/
  view.cpp / view_group.cpp / view_root.cpp / scroll_view.cpp
  window.cpp     窗口组件（标题栏 + 内容区）
  layout_inflater.cpp 布局注册表查找
  font.cpp       字体集
  layouts/       布局实现
  widgets/       控件实现
  os/windows/    Windows 后端：窗口、GDI+ 绘制、图标、标题栏外观
  os/macos/      预留（尚未实现）
  os/linux/      预留（尚未实现）
tools/layoutgen/ XML → C++ 布局代码生成器
cmake/           aiui_add_layouts() 构建规则
```

## 四层渲染核心

对应 Android 的 `View` / `ViewGroup` / `ViewRootImpl`，整棵界面树共用**一个**
原生窗口：

| 层 | 职责 |
| --- | --- |
| `View` | 一块矩形区域：测量、绘制、事件、刷新 |
| `ViewGroup` | 管子树的增删、摆放、命中测试与逐层绘制 |
| `ViewRoot` | 唯一持有窗口与画布，按系统更新区重绘，统一管焦点 / 捕获 / 光标 |
| `ScrollView` | 可滚动视图基类，自绘滚动条、处理滚轮与滑块拖动 |

子视图不创建任何原生句柄，`WM_PAINT` 只由 `ViewRoot` 收一次，再按脏区
递归绘制整棵树。因此拖动窗口、超出屏幕、跨显示器都不会留下空白。

坐标约定：

- `bounds()` 相对父容器，`clientRect()` 恒为 `{0,0,w,h}`
- 控件内部一律用自身局部坐标，由 `ViewGroup` 负责 `painter.translate()`
- `invalidateRect()` 收局部坐标，`ViewRoot::invalidateRect()` 收窗口坐标

## 绘制分层

| 层 | 职责 |
| --- | --- |
| `PaintBackend` | 平台最小图元：填充、圆角、描边、渐变、文字、图标、位图、裁剪、平移 |
| `Painter` | 组合逻辑：圆角、渐变、尾部省略号、图标 + 文字排版 |
| `Canvas` | GDI+ 位图离屏绘制，只覆盖脏区，一次性贴回，避免闪烁与半帧 |

控件只依赖 `Painter`，换平台时重写 `PaintBackend` 即可，控件代码一行不改。

## 布局

布局即容器控件，`FrameLayout` / `LinearLayout` 继承 `ViewGroup`，
覆盖 `onMeasure` / `arrangeChildren`：

```cpp
auto* column = new LinearLayout(Orientation::kVertical);
column->addView(toolBar, LayoutParams{}.withSize(LayoutParams::kMatchParent, 52));
column->addView(list, LayoutParams{}.withWeight(1.0f));
```

- `LayoutParams::width/height`：`kWrapContent` 按内容、`kMatchParent` 撑满、
  其余为 96 DPI 设计值
- `weight`：`LinearLayout` 里按比例分配剩余空间，`0` 表示不参与拉伸
- 所有尺寸在设计期按 96 DPI 书写，运行时由 `View::px()` 换算

## XML 布局

构建期把 `.xml` 转成 C++ 类并加入编译，用户侧无需额外步骤：

```cmake
aiui_add_layouts(AiZip assets/layouts/main_frame.xml)
```

生成的类在 `AiUI::generated` 命名空间，类名由文件名 PascalCase 派生
（`main_frame.xml` → `MainFrame`），带 `id` 的节点成为成员指针：

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

- 标签名就是控件类名；AiUI 内置控件不需要额外属性
- 自定义控件用 `header` 指明头文件、`ns` 指明命名空间
- 根节点必须是 `LinearLayout` 或 `FrameLayout`

## 事件

控件自己持有数据、自己绘制，调用方只传数据和回调，不需要继承控件：

```cpp
// ListView：库只保存行数，滚到哪一行才向适配器要数据
list->setColumns({{L"名称", 320}, {L"类型", 110}});

auto source = std::make_shared<ListRowSource>();
source->setCount([&] { return static_cast<int>(items.size()); });
source->setBinder([](int row, ListRow& out) {
  out.tag = items[row].index;
  out.icon = ListIcon{.glyph = IconId::kFolder, .color = kColorTextFaint};
  out.cells = {{items[row].name}, {items[row].typeName}};
});
list->setAdapter(source);
list->setCallbacks({.onActivate = [](int row) { /* ... */ }});

// 数据变了：只通报，不传整表
list->notifyDataSetChanged();
```

- `ListAdapter` 是桥：`rowCount()` 供行数，`bindRow()` 把某一行摊平到 `ListRow`，`tagAt()` 回业务标识
- `ListRowSource` 是函数式实现，不用建类；`ListArrayAdapter` 适合数据本来就在库这边的情况
- 可视行由 `ListViewHolder` 承载，滚动时换绑行号、复用对象与字符串容量，长列表不反复分配
- 内容整体变了用 `notifyDataSetChanged`，只有少数行变了用 `notifyRowsChanged`

| 控件 | 传数据 | 交互回调 |
| --- | --- | --- |
| `ListView` | `setColumns` / `setAdapter` / `notifyDataSetChanged` | `onActivate` / `onSelectionChanged` / `onSort` / `onContextMenu` |
| `TreeView` | `setRoot` / `setChildrenLoader` | `setSelectHandler` |
| `BreadcrumbBar` | `setSegments` | `onNavigate` / `onBack` / `onSearch` / `onCommitAddress` |
| `StatusBar` | `setFields` | — |
| `ButtonBar` | `addButton(ButtonSpec)` | `Button::setOnClick` |

控件内部按需局部重绘：悬停、选中只重画受影响的行，滚动只重画行区，
不触发整控件或整窗刷新。

## 事件分发

`ViewRoot` 命中到最深的可交互视图后**直接投递**，不逐层转发：

- 按下：命中视图收到 `onMouseDown`，并沿祖先链给焦点；点到不可聚焦控件会让输入框失焦
- 抬起：一定回到按下的那个视图，拖出控件外松手也能正确结束
- 悬停：`hovered_` 只保留一个，进入 / 离开各一次
- 滚轮：命中视图不消费就沿祖先链冒泡，外层 `ScrollView` 接手
- 捕获：拖动滚动条 / 列宽期间事件全部给捕获者

## 控件生命周期

`create()` 建窗口并递归挂载子树 → `onAttach()` → `onCreated()` →
`measure()` 求尺寸 → `layout()` 摆放 → `draw()` 自绘 → 事件回调。
子视图所有权归父容器，父容器析构时递归释放。

也支持派生覆盖（少数需要完全自定义绘制的场景）：

```cpp
class MyGauge : public View {
  void onPaint(Painter& painter, const Rect& rect) override;
};
```

## 构建

```bat
cmake -S AiUI -B AiUI\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build AiUI\build --parallel
```

单独构建得到静态库与头文件，可供其它宿主工程复用。
