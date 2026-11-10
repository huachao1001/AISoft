# TextView

单行文字。支持对齐、尾部省略号、粗体、小字号和四边内边距。

```cpp
#include "AiUI/widgets/text_view.h"

auto* label = new TextView(L"名称");

TextStyle style;
style.color = kColorTextHead;
style.align = TextAlign::kLeft;
style.bold = true;
label->setTextStyle(style);
```

## TextStyle

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `color` | `kColorText` | 文字颜色 |
| `align` | `kLeft` | 左 / 居中 / 右 |
| `ellipsis` | `true` | 超宽时显示尾部省略号 |
| `bold` | `false` | 用加粗字体 |
| `small` | `false` | 用小字号 |
| `paddingLeft/Top/Right/Bottom` | 0 | 四边内边距（设计值） |

字体选择规则：`small` 优先于 `bold`。同时为 true 时用 `uiSmall`，
不是"小号加粗"——库里只有三套字体（`ui` / `uiBold` / `uiSmall`）。

## 方法

| 方法 | 说明 |
| --- | --- |
| `setText(const std::wstring&)` | 换文字；内容相同则不重绘 |
| `text()` | 当前文字 |
| `setTextStyle(const TextStyle&)` | 整体替换样式 |
| `setColor(Color)` | 只改颜色 |
| `setAlign(TextAlign)` | 只改对齐 |

构造时可直接给文字：`new TextView(L"标题")`。

## 省略号

`ellipsis = true` 且 `align == kLeft` 时，超宽文字按尾部省略号处理。
右对齐和居中的文字超宽时不做省略——这两种对齐下省略号的位置语义
不清晰，所以直接按对齐裁切。

需要严格保证不裁切时，把 `ellipsis` 设为 false，并给足够宽度。

## 测量

`onMeasure` 返回文字的期望尺寸加内边距。只支持单行，遇到换行符按普通
字符处理，不会真的换行。

## XML

```xml
<TextView id="title" text="名称" textColor="#333333" align="left" bold="true"/>
```

| 属性 | 说明 |
| --- | --- |
| `text` | 文字；也可以直接写标签内容 |
| `textColor` | 文字颜色，`#rrggbb` 或 `#aarrggbb` |
| `align` | `left` / `center` / `right` |
| `bold` | `true` / `false` |
| `small` | `true` / `false` |
| `ellipsis` | `false` 关闭省略号 |
| `padding` / `paddingX` | 四边 / 水平内边距 |

标签内容也可以当文字：

```xml
<TextView textColor="#666666">共 128 项</TextView>
```

`text` 属性优先于标签内容；两者都空则不显示。

## 注意

- 单行控件，不支持换行、富文本或自动折行。
- 每改一次样式都会整控件重绘，批量改建议先组装好 `TextStyle` 再一次性设置。
