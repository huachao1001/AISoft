# ButtonBar

横向按钮条。按钮按内容宽度从左往右排，分组之间留更宽的间距并画一条竖
分隔线。工具条就是它的典型用法。

```cpp
#include "AiUI/widgets/button_bar.h"

auto* bar = new ButtonBar();
bar->addButton(ButtonSpec{.label = L"刷新", .icon = IconId::kRefresh, .hasIcon = true}, 0);
bar->addButton(ButtonSpec{...}, 0);
bar->addButton(ButtonSpec{...}, 1);   // group 变了，前面插分隔线
```

## 方法

| 方法 | 说明 |
| --- | --- |
| `addButton(const ButtonSpec&, int group = 0)` | 追加按钮，返回按钮指针用于挂回调 |
| `buttonAt(int)` | 按下标取按钮，越界返回 `nullptr` |
| `clear()` | 移除并销毁全部按钮 |
| `relayout()` | 重新排布，改完按钮状态后调用 |
| `setEdgePad(int)` | 左侧起始留白（设计值，默认 16） |
| `setGap(int)` | 同组按钮间距（默认 8） |

`addButton()` 内部已经调了 `relayout()`，通常不需要手动调。

## 分组

`group` 是一个整数标记，只用来判断"相邻两项是否同组"。组号变化时：

- 间距从 `gap` 换成更宽的组间距（默认 10）
- 在两者正中间画一条竖线

组号不需要连续，`0, 0, 5, 5` 和 `0, 0, 1, 1` 效果一样。

```cpp
bar->addButton(refreshSpec, 0);
bar->addButton(extractSpec, 0);
bar->addButton(compressSpec, 1);   // 分隔线出现在这里
bar->addButton(addSpec, 1);
```

## 宽度

按钮条的按钮**一律按内容自适应宽度**，`ButtonSpec::width` 在这个容器里
不生效。要控制按钮宽度，只能调 `padX` / `iconGap` / `iconSize`，或者
换文字。

条自身高度固定 `kSizeToolH`（52），按钮在条内垂直居中。

## XML

```xml
<ButtonBar id="toolBar" height="52"/>
```

按钮条在 XML 里只能是空壳，按钮必须在代码里装配。原因有两个：

- 按钮的置灰条件、文字、图标都跟运行时状态有关。
- `ButtonBar::addButton` 的签名是 `addButton(ButtonSpec, group)`，不是容器通用的
  `addView(View*, LayoutParams)`，所以在 XML 里嵌套 `<Button>` 会编译失败。

## 竖分隔线

分隔线在 `onPaint` 里按 `groupLines_` 批量画，颜色固定 `kColorLine`。
条底部还会画一条横向分隔线。

## 注意

- 按钮顺序就是 `addButton` 的顺序，没有排序或动态插入接口；要调整请 `clear()` 重建。
- 内容宽度超出容器时不会自动换行或折行，按钮会被裁掉。
