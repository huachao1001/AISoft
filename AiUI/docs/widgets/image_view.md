# ImageView

显示位图或矢量图标。位图由平台解码器加载，支持 `png` / `jpeg` / `gif` /
`bmp` / `ico` / `webp`。

```cpp
#include "AiUI/widgets/image_view.h"

auto* view = new ImageView();
if (!view->loadFile(L"assets/logo.png")) {
  view->setIcon(IconId::kFolder);   // 加载失败退回矢量图标
}
view->setScale(ImageScale::kFit);
```

## 方法

| 方法 | 说明 |
| --- | --- |
| `loadFile(const std::wstring&)` | 从文件加载；失败返回 `false` 并清空当前内容 |
| `setImage(const Image&)` | 直接给已解码的位图 |
| `setIcon(IconId)` | 用内建矢量图标，会清掉位图 |
| `clearImage()` | 清空，什么都不画 |
| `hasImage()` | 是否有内容可画 |
| `setScale(ImageScale)` | 适配方式 |
| `setPlaceholderColor(Color)` | 矢量图标的颜色 |

`setImage` 和 `setIcon` 互斥：设了图标的会被设位图清掉，反之亦然。
`loadFile` 失败时当前内容被清空，不会保留旧图。

## ImageScale

| 枚举 | 效果 |
| --- | --- |
| `kFit` | 等比缩放到完全放得下（默认） |
| `kFill` | 等比铺满，超出部分裁掉 |
| `kStretch` | 拉伸到目标大小，不保持比例 |
| `kCenter` | 原始大小居中 |

当前实现里位图的原始像素尺寸没有回填到控件（`sourceSize_` 保持为 0），
所以：

- `kFit` 会退化成铺满整个区域；
- `kCenter` 直接按容器尺寸计算，等价于铺满；
- `kStretch` 和 `kFill` 本来就是铺满区域。

**实际只有 `kStretch` / `kFill` 的语义是准确的**，需要严格等比留白时
请显式给控件设置与图片同比例的尺寸。

矢量图标不受影响：它总是按短边贴合成正方形绘制在中央。

## 测量

因为拿不到位图原始尺寸，`onMeasure` 总是返回可用空间——`ImageView`
会撑满父容器给的空间。所以通常要显式写 `width` / `height`。

## XML

```xml
<ImageView id="logo" src="assets/logo.png" width="64" height="64" scale="fit"/>
<ImageView id="badge" icon="home" width="24" height="24" tint="#666666"/>
```

| 属性 | 说明 |
| --- | --- |
| `src` | 位图路径 |
| `icon` | 矢量图标名 |
| `scale` | `fit`（默认）/ `fill` / `stretch` / `center` |
| `tint` | 矢量图标颜色，对应 `setPlaceholderColor` |

`src` 和 `icon` 都存在时，`src` 先加载、`icon` 后设置，最终显示图标。

## 注意

- 位图句柄由平台缓存或调用方持有，`ImageView` 不负责释放。
- 透明区域会直接透出父控件底色（没有额外背景）。
- 没有图片时控件不画任何东西，不会显示占位框。
