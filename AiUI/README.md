# AiUI

自绘 UI 库。控件不创建系统子控件，所有像素都由 GDI+ 画出来，平台相关代码
收在 `src/win32/`，公开头文件不出现 `HWND` / `HDC` / `COLORREF`。

## 架构

```
include/AiUI/
  types.h        基础类型：Rect / Point / Size / Color / Gravity
  events.h       中性事件：MouseEvent / Key / Modifiers / ScrollEvent
  platform.h     平台后端接口（窗口、字体、图片、绘制子系统）
  painter.h      PaintBackend（最小图元）+ Painter（组合绘制）+ Canvas（双缓冲）
  theme.h        配色与尺寸常量（96 DPI 设计值）
  icon.h         矢量图标 ID
  font.h         FontSet：按 DPI 创建的一组界面字体
  widget.h       控件基类：树结构、测量、布局、事件分发
  layouts/       FrameLayout / LinearLayout
  widgets/       Button / ButtonBar / InputBox / ListView / TreeView /
                 StatusBar / TextView / ImageView
src/
  widget.cpp     控件基类实现
  font.cpp       字体集
  layouts/       布局实现
  widgets/       控件实现
  win32/         平台后端：窗口、GDI+ 绘制、图标、位图
tools/layoutgen/ XML → C++ 布局代码生成器
cmake/           aiui_add_layouts() 构建规则
```

## 绘制分层

| 层 | 职责 |
| --- | --- |
| `PaintBackend` | 平台最小图元：填充、圆角、描边、渐变、文字、图标、位图、裁剪 |
| `Painter` | 组合逻辑：圆角、渐变、尾部省略号、图标 + 文字排版 |
| `Canvas` | GDI+ 位图离屏绘制，一次性贴回，避免闪烁与半帧 |

控件只依赖 `Painter`，换平台时重写 `PaintBackend` 即可，控件代码一行不改。

## 布局

布局即容器控件，`FrameLayout` / `LinearLayout` 继承 `Widget`，
覆盖 `onMeasure` / `arrangeChildren` / `onPaint`。

```cpp
auto* column = new LinearLayout(Orientation::kVertical);
column->add(toolBar, LayoutParams{}.withSize(LayoutParams::kMatchParent, 52));
column->add(list, LayoutParams{}.withWeight(1.0f));
```

- `LayoutParams::width/height`：`kWrapContent` 按内容、`kMatchParent` 撑满、
  其余为 96 DPI 设计值
- `weight`：`LinearLayout` 里按比例分配剩余空间，`0` 表示不参与拉伸
- 所有尺寸在设计期按 96 DPI 书写，运行时由 `Widget::px()` 换算

## XML 布局

构建期把 `.xml` 转成 C++ 类并加入编译，用户侧无需额外步骤：

```cmake
aiui_add_layouts(AiZip assets/layouts/main_frame.xml)
```

生成的类在 `AiUI::generated` 命名空间，类名由文件名 PascalCase 派生
（`main_frame.xml` → `MainFrame`），带 `id` 的节点成为成员指针：

```xml
<LinearLayout orientation="vertical">
  <CrumbBar id="crumb" ns="ui" header="ui_crumb.h" height="48"/>
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

两种风格可以混用：

```cpp
// 回调
button->setOnClick([] { /* ... */ });
input->setOnChange([this] { SetFilterText(input->text()); });
// 或派生覆盖
class MyList : public ListView {
  bool onKeyDown(const Key& key, const Modifiers& mods) override;
};
```

## 控件生命周期

`create()` 建平台窗口 → `measure()` 求尺寸 → `layout()` 摆放 → `paint()`
自绘 → 事件回调。子控件所有权归父控件，父控件析构时递归释放。

## 构建

```bat
cmake -S AiUI -B AiUI\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build AiUI\build --parallel
```

单独构建得到静态库与头文件，可供其它宿主工程复用。
