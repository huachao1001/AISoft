# BreadcrumbBar

后退按钮 + 路径节点 + 搜索框，三部分全部自绘。

调用方只传路径段，节点宽度、省略策略、命中区都在内部算。地址栏的编辑
输入框由组件自己创建并托管，业务侧不用管。

```cpp
#include "AiUI/widgets/breadcrumb_bar.h"

std::vector<BreadcrumbSegment> segments;
segments.push_back(BreadcrumbSegment{.label = L"C:", .tag = L"C:\\", .home = true});
segments.push_back(BreadcrumbSegment{.label = L"Users", .tag = L"C:\\Users"});
segments.push_back(BreadcrumbSegment{.label = L"chaohua", .tag = L"C:\\Users\\chaohua",
                                     .current = true});
crumb->setSegments(std::move(segments));
```

## BreadcrumbSegment

| 字段 | 说明 |
| --- | --- |
| `label` | 显示文字 |
| `tag` | 业务标识，点击时原样回传 |
| `current` | 当前所在层，画成高亮底且点击不触发导航 |
| `home` | 段首画一个房子图标 |

`tag` 是 `std::wstring` 而不是整型，因为路径本身是字符串；业务侧不必再
维护一份下标映射。

## 回调

```cpp
BreadcrumbCallbacks callbacks;
callbacks.onBack = [] { GoUp(); };
callbacks.onNavigate = [](const std::wstring& tag) { NavigateTo(tag); };
callbacks.onSearch = [](const std::wstring& text) { Filter(text); };
callbacks.addressText = [] { return CurrentPath(); };        // 进入编辑时填什么
callbacks.onCommitAddress = [](const std::wstring& text) { Commit(text); };
callbacks.onEditClosed = [] { FocusMainView(); };
bar->setCallbacks(std::move(callbacks));
```

| 回调 | 触发时机 |
| --- | --- |
| `onBack` | 点后退按钮，且 `setBackEnabled(true)` |
| `onNavigate` | 点非 `current` 节点，参数是该节点的 `tag` |
| `onSearch` | 搜索框内容变化 |
| `addressText` | 进入地址编辑态时被调用，返回值填进输入框 |
| `onCommitAddress` | 地址编辑按回车提交 |
| `onEditClosed` | 编辑态结束（提交或失焦），用于把焦点交回主视图 |

## 方法

| 方法 | 说明 |
| --- | --- |
| `setSegments(std::vector<BreadcrumbSegment>)` | 整组替换路径节点 |
| `setStyle(const BreadcrumbStyle&)` | 覆盖默认视觉参数 |
| `setCallbacks(BreadcrumbCallbacks)` | 挂回调 |
| `setBackEnabled(bool)` | 后退按钮是否可用（默认不可用，画成灰色） |
| `setSearchCue(const std::wstring&)` | 搜索框提示文字 |
| `clearSearch()` | 清空搜索框内容 |
| `beginEdit()` | 进入地址编辑态 |
| `isEditing()` | 当前是否在编辑地址 |
| `focusSearch()` | 把焦点交给搜索框 |

## 交互

- **点后退** → `onBack`
- **点路径节点** → `onNavigate`；点到 `current` 节点不响应
- **点空白处** → 进入地址编辑态
- **编辑态按回车** → `onCommitAddress`
- **编辑态按 Esc 或失焦** → 丢弃输入，`onEditClosed`

`focusSearch()` 会先退出编辑态再聚焦搜索框，两者互斥。

## 节点放不下时

节点从后退按钮右侧开始排，可用的右边界是"搜索框左边减 8px"。一旦某个
节点越过这条线，它**和它之后的所有节点**都不再绘制，也不可点击。

这意味着：路径很深时，末尾的 `current` 节点可能反而看不见。调用方应当在
传 `segments` 之前自己控制深度，或者只传末几段。

## BreadcrumbStyle

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `padX` | 16 | 左右内边距 |
| `nodePadX` / `nodePadY` | 7 / 4 | 节点内边距 |
| `nodeGap` | 8 | 节点与分隔箭头的间距 |
| `backWidth` / `backHeight` | 30 / 30 | 后退按钮尺寸 |
| `iconSize` / `iconGap` | 16 / 6 | 房子图标尺寸与间距 |
| `radius` | 5 | 节点圆角 |
| `searchWidth` / `searchHeight` | 270 / 34 | 搜索框尺寸 |
| `editHeight` | 32 | 地址编辑框高度 |

搜索框宽度会自适应：`max(120, min(searchWidth, 可用宽度))`。

## XML

```xml
<BreadcrumbBar id="crumb" height="48"/>
```

路径段和回调都要在代码里配，XML 只描述位置和高度。

## 注意

- `setSegments` 会立即重算命中区，否则新节点没有可点击区域。
- 编辑框和搜索框是组件的子控件，随组件一起释放。
- 编辑框的圆角由 `InputBox` 自己画，`outerBackground` 取面包屑的底色，
  换底色时记得同步 `BreadcrumbStyle::background`。
