# StatusBar

底栏文字区。左侧显示当前位置（路径或包内路径）、可选的格式标签和大小，
右侧显示统计信息。

```cpp
#include "AiUI/widgets/status_bar.h"

StatusFields fields;
fields.left = L"C:\\Users\\chaohua\\Documents";
fields.tag = L"ZIP";
fields.size = L"12.4 MB";
fields.right = L"共 128 项，已选 3 项";
fields.showHome = true;
bar->setFields(fields);
```

## StatusFields

| 字段 | 说明 |
| --- | --- |
| `left` | 路径或包路径 |
| `tag` | 格式标签，空则不画 |
| `size` | 大小 |
| `right` | 右侧统计 |
| `showHome` | 左侧是否画一个房子图标 |

四个文本字段都是空串安全——空了就不画，不需要在调用方判断。

## 布局

底部一行，从左往右依次是：房子图标 → 路径 → 格式标签 → 大小；右侧统计
独立右对齐。

右侧统计先量宽度，作为左侧内容的右边界。左侧放不下时，路径**从左侧
逐字裁掉**（而不是从尾部加省略号），保证标签和大小不会被挤压移位。

标签有独立的背景胶囊和文字色（`kColorTagBg` / `kColorTagInk`），和正文
区分开。标签宽度超过剩余空间时直接不画。

## StatusStyle

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `padX` | 16 | 左右内边距 |
| `homeIconSize` | 15 | 房子图标尺寸 |
| `tagPadX` | 6 | 标签左右内边距 |
| `tagHeight` / `tagRadius` | 19 / 4 | 标签胶囊高度与圆角 |
| `background` | `kColorTool` | 底色 |
| `text` | `kColorTextMuted` | 文字色 |
| `divider` | `kColorLine` | 顶部分隔线 |

## 测量

`onMeasure` 返回 `{0, 36}`（`kSizeStatusH`）。宽度由父容器决定，高度可被
`LayoutParams` 覆盖。

## XML

```xml
<StatusBar id="status" height="36"/>
```

所有内容都要在代码里 `setFields`。

## 注意

- 只有一行，不支持多行或换行。
- 字段之间没有交互，没有点击回调；需要点击请自行叠加控件。
- 左侧路径被裁时先裁头部，这与"路径末尾更有信息量"的直觉一致。
