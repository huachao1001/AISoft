# StatusBar

底栏文字区。左侧显示当前位置（路径或包内路径）、可选的格式标签和大小，
右侧显示统计信息。

```cpp
#include "AiUI/widgets/status_bar.h"

StatusFields fields;
fields.left = L"C:\\Users\\chaohua\\Documents";
fields.tag = L"ZIP";
fields.badges = {L"1600×1000", L"8.4 KB", L"1/3"};
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
| `badges` | 追加的胶囊标签（分辨率、大小、序号等），逐个画成与 `tag` 同款 |
| `size` | 大小（纯文字，不带胶囊底） |
| `right` | 右侧统计 |
| `showHome` | 左侧是否画一个房子图标 |

四个文本字段都是空串安全——空了就不画，不需要在调用方判断。
`badges` 里的空串会被跳过，调用方可以按条件直接塞入。

## 布局

底部一行，从左往右依次是：房子图标 → 路径 → 格式标签 → `badges` 胶囊
→ 大小；右侧统计独立右对齐。

右侧统计先量宽度，作为左侧内容的右边界。左侧放不下时优先保住胶囊：
先量出所有胶囊的总宽，路径只拿剩下的部分，放不下就从左侧逐字裁掉
（而不是从尾部加省略号）。胶囊本身按完整宽度画，放不下的那一颗直接
不画——半截的「1600×…」没有任何用处。

标签与胶囊有独立的背景胶囊和文字色（`kColorTagBg` / `kColorTagInk`），
和正文区分开。

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
