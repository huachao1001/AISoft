# Button

圆角描边按钮，支持图标 + 文字。悬停、按下、禁用三种状态的外观都在控件
内部解析，调用方只给一个 `ButtonSpec`。

```cpp
#include "AiUI/widgets/button.h"

ButtonSpec spec;
spec.label = L"刷新";
spec.icon = IconId::kRefresh;
spec.hasIcon = true;

auto* button = new Button();
button->setSpec(spec);
button->setOnClick([] { /* ... */ });
```

## ButtonSpec

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `label` | 空 | 按钮文字 |
| `icon` | `kNone` | 矢量图标 |
| `hasIcon` | `false` | **必须显式置 true 才画图标** |
| `placement` | `kLeft` | 图标位置，见下 |
| `style` | `kOutline` | 描边 / 弱化 / 平铺 / 强调 |
| `width` | `0` | 0 表示按内容自适应 |
| `height` | `32` | 按钮高度 |
| `iconSize` | `18` | 图标边长 |
| `padX` | `12` | 左右内边距 |
| `iconGap` | `6` | 图标与文字间距 |
| `radius` | `7` | 圆角半径 |
| `background` | `kNoColor` | 显式底色，优先于状态色 |
| `border` | `kNoColor` | 显式边框色 |
| `ink` | `kNoColor` | 显式文字/图标色 |

`kNoColor` 是"交给主题决定"的哨兵值，不是透明的意思。

## 图标位置

| 枚举 | 效果 |
| --- | --- |
| `IconPlacement::kLeft` | 图标在文字左侧（默认） |
| `IconPlacement::kTop` | 图标在文字上方，竖排 |
| `IconPlacement::kOnly` | 只画图标，不画文字 |
| `IconPlacement::kNone` | 只画文字，即使 `hasIcon` 为 true |

## 外观风格

| 枚举 | 默认态 | 悬停 | 按下 |
| --- | --- | --- | --- |
| `ButtonStyle::kOutline` | 白底 + 派生描边 | 浅蓝底 + 略深描边 | 深蓝底 |
| `ButtonStyle::kSubtle` | 无底无边框 | 浅灰底 | 深蓝底 |
| `ButtonStyle::kFlat` | 无底无边框 | 浅灰底 | 深蓝底 |
| `ButtonStyle::kPrimary` | 强调色实心 | 强调色提亮 | 强调色加深 |

`kPrimary` 用主题的 `accent` / `accentHover` / `accentDown`，文字取 `accentInk`。
一个界面只应有一个 `kPrimary`：两个同等分量的实心按钮会让人不知道点哪个。

`kSubtle` 与 `kFlat` 当前绘制效果相同，区别只在语义。

`kOutline` 的描边色由最终底色经 `edgeOn()` 派生（浅底叠淡墨、深底叠淡白），
不是写死的灰：换主题或换显式底色时不会出现"深底上一根亮线"。`spec.border`
仍可覆盖这一派生结果。

禁用时三种风格统一换成浅灰底 + 浅灰文字，且清掉悬停与按下状态。

## 方法

| 方法 | 说明 |
| --- | --- |
| `setSpec(const ButtonSpec&)` | 整体替换外观配置 |
| `setLabel(const std::wstring&)` | 只改文字 |
| `setIcon(IconId)` | 只改图标；传 `kNone` 会同时关掉 `hasIcon` |
| `setEnabled(bool)` | 禁用/启用，继承自 `View` |
| `preferredWidth()` | 按内容算出的宽度（物理像素） |

## 宽度自适应

`width == 0` 时宽度由 `preferredWidth()` 算出：

```
padX * 2 + iconSize（有图标时）+ iconGap（图标和文字都有时）+ 文字宽度
```

文字宽度走 `Platform::measureText`，不依赖原生窗口——所以布局阶段
（控件还没挂上窗口）也能正确测量。

## 点击

```cpp
button->setOnClick([] { DoSomething(); });
```

`setOnClick` 是 `View` 的通用回调，在鼠标抬起且仍在按钮内部时触发。
双击不会触发两次，`onDoubleClick` 只会让按钮退出按下态。

## XML

```xml
<Button id="refresh" text="刷新" icon="refresh" style="outline" height="32"/>
```

| 属性 | 说明 |
| --- | --- |
| `text` | 文字 |
| `icon` | 图标名，见下表 |
| `iconPlacement` | `left`（默认）/ `top` / `only` / `none` |
| `style` | `outline`（默认）/ `subtle` / `flat` |
| `width` / `height` | 尺寸；`width` 不写则自适应 |
| `iconSize` / `radius` / `padX` / `iconGap` | 尺寸微调 |
| `textColor` | 对应 `spec.ink` |
| `backgroundColor` / `borderColor` | 对应 `spec.background` / `spec.border` |

可用的图标名：`refresh`、`extract`、`extractAll`、`add`、`trash`、
`arrowBack`、`chevron`、`chevronUp`、`chevronDown`、`home`、`folder`、
`file`、`search`。

XML 里写了 `icon` 会自动置 `hasIcon`，代码里手写 `ButtonSpec` 则要自己置。

## 注意

- 按钮自身不参与 `weight` 分配，尺寸由 `preferredWidth()` 或 `spec.width` 决定。
- 放进 `ButtonBar` 时宽度一律按 `preferredWidth()` 计算，`spec.width` 会被忽略。
