# LinearLayout

子控件按单一方向依次排列，剩余空间按 `weight` 比例分配。语义与 Android
的 `LinearLayout` 一致。

布局即容器控件，`LinearLayout` 直接继承 `ViewGroup`，可以当普通控件用。

```cpp
#include "AiUI/layouts/linear_layout.h"

auto* column = new LinearLayout(Orientation::kVertical);
column->setSpacing(8);
column->setPadding(16, 12);

column->addView(header, LayoutParams{}.withSize(LayoutParams::kMatchParent, 48));
column->addView(body, LayoutParams{}.withWeight(1.0f));
column->addView(footer, LayoutParams{}.withSize(LayoutParams::kMatchParent, 36));
```

## 构造与方向

```cpp
explicit LinearLayout(Orientation orientation = Orientation::kVertical);
void setOrientation(Orientation orientation);
```

| 枚举 | 主轴 | weight 拉伸 |
| --- | --- | --- |
| `Orientation::kVertical`（默认） | 从上到下 | 高度 |
| `Orientation::kHorizontal` | 从左到右 | 宽度 |

## 属性

| 方法 | 说明 |
| --- | --- |
| `setSpacing(int)` | 相邻子控件间距（设计值） |
| `setGravity(Gravity)` | 交叉轴对齐：`kStart` / `kCenter` / `kEnd` |
| `setPadding(int)` | 四边等宽内边距 |
| `setPadding(int h, int v)` | 水平、垂直内边距 |
| `setPadding(int l, int t, int r, int b)` | 分别指定四边 |
| `addView(View*, LayoutParams)` | 追加子视图，返回子视图指针 |
| `addChildView(View*, LayoutParams)` | 同上，返回容器自身便于链式调用 |

`Gravity` 只影响交叉轴。纵向排列时它控制水平对齐，横向排列时控制垂直对齐。

## weight 的分配规则

排布分两遍：

1. 先量出所有子控件的期望尺寸。`weight == 0` 的子控件按自身尺寸占位，
   之和记为 `used`。
2. 剩余空间 `available - used` 按各子控件 `weight` 占比分给 `weight > 0` 的子控件。

```cpp
// 左树固定 230，列表吃掉剩下的全部宽度
content->addView(tree, LayoutParams{}.withSize(230, LayoutParams::kMatchParent));
content->addView(list, LayoutParams{}.withWeight(1.0f));
```

注意几点：

- 带 `weight` 的子视图**不再使用**自身期望尺寸，完全由分到的空间决定。
- `weight` 只在主轴上生效，交叉轴由 `kMatchParent` 或 `Gravity` 决定。
- 多个 `weight` 时按比例分配：`weight(1)` 和 `weight(2)` 得到 1:2。
- 剩余空间为负时按 0 处理，子控件不会被压成负数。

## 测量

`onMeasure` 返回"所有子视图主轴尺寸之和 + 间距 + 内边距"，交叉轴取最大值。
父容器给 `kMatchParent` 时会覆盖这个结果。

## XML

```xml
<LinearLayout orientation="vertical" spacing="8" padding="16" gravity="center">
  <TextView text="标题" height="48"/>
  <ListView weight="1" height="match"/>
</LinearLayout>
```

| 属性 | 说明 |
| --- | --- |
| `orientation` | `vertical`（默认）/ `horizontal` |
| `spacing` | 子控件间距 |
| `gravity` | `start` / `center` / `end` |
| `padding` | 四边等宽内边距 |

`weight`、`marginX`、`marginY`、`width`、`height` 属于 `LayoutParams`，
写在与其它控件相同的属性位置。只有 `padding` 是容器自身的四边等宽版本；
分别指定四边要在代码里调 `setPadding(l, t, r, b)`。

## 限制

- 不滚动。内容超出容器就溢出，需要滚动请用 `ListView` 或 `TreeView`。
- 不支持 `wrap_content` 的循环依赖：父给 `wrap`、子也要 `wrap` 时按 0 处理。
