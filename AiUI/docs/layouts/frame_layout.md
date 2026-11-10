# FrameLayout

子控件按绝对坐标叠放，后加的画在上层。语义与 Android 的 `FrameLayout`
一致——适合做"层叠"而不是"排列"。

```cpp
#include "AiUI/layouts/frame_layout.h"

auto* frame = new FrameLayout();
frame->setPadding(16);

frame->addView(background, LayoutParams::match());
frame->addView(badge, LayoutParams::at(12, 12, 24, 24));
frame->addView(hint, LayoutParams{}.withSize(LayoutParams::kWrapContent, LayoutParams::kWrapContent)
                                 .align(Gravity::kCenter, Gravity::kEnd));
```

## 属性

| 方法 | 说明 |
| --- | --- |
| `setPadding(int)` | 四边等宽内边距 |
| `setPadding(int h, int v)` | 水平、垂直内边距 |
| `setPadding(int l, int t, int r, int b)` | 分别指定四边 |
| `addView(View*, LayoutParams)` | 追加子视图，返回子视图指针 |
| `addChildView(View*, LayoutParams)` | 同上，返回容器自身便于链式调用 |

## 定位规则

每个子控件的位置按以下顺序决定：

1. 起点 = 内容区左上角 + 外边距。
2. `LayoutParams::x` / `y` 非 0 时，叠加这个偏移，不再看对齐方式。
3. `x` 为 0 时按 `horizontal` 对齐：`kStart` 贴左、`kCenter` 居中、`kEnd` 贴右。
4. `y` 为 0 时按 `vertical` 对齐，规则同上。

因为 `x == 0`、`y == 0` 被当作"未指定"，要把控件精确放到左上角时，
用 `at(0, 0, w, h)` 与直接不写坐标效果相同，都落在内边距左上角。

```cpp
// 右下角浮一个角标
frame->addView(corner, LayoutParams{}.withSize(24, 24).align(Gravity::kEnd, Gravity::kEnd));
```

## 测量

`onMeasure` 取所有子视图"期望尺寸 + 偏移"的最大值，再加上内边距。
这意味着 `FrameLayout` 会倾向于包住最大的那个子视图。

## 尺寸

| 值 | 行为 |
| --- | --- |
| `LayoutParams::kMatchParent` | 撑满容器内容区 |
| `LayoutParams::kWrapContent` | 用子控件自己的期望尺寸 |
| 具体数值 | 按 96 DPI 换算成物理像素 |

## XML

```xml
<FrameLayout padding="16">
  <ImageView src="assets/logo.png" width="64" height="64" align="center" alignY="center"/>
  <TextView text="版本 1.0" width="wrap" height="wrap" align="end" alignY="end"/>
</FrameLayout>
```

| 属性 | 说明 |
| --- | --- |
| `padding` | 四边等宽内边距 |
| `x` / `y` | 相对内容区左上角的绝对偏移 |
| `align` / `alignY` | 水平 / 垂直对齐：`start` / `center` / `end` |

## 何时该用

- 需要叠放：背景 + 内容 + 浮层。
- 需要精确坐标：自绘面板里画控件。
- 只是要"横排或竖排"：用 `LinearLayout`，`FrameLayout` 需要手动算每个位置。

## 限制

- 子控件之间不会自动避让，重叠是允许的（也是它存在的意义）。
- 没有 `z` 轴控制，层级就是 `add` 的先后顺序。
