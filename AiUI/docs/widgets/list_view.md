# ListView

自绘虚拟列表：带表头、可排序、可拖列宽、多选、滚动。表头和每一行都由
控件自己画，不创建 `SysListView32` / `SysHeader32`。

核心设计参考 `RecyclerView`：**控件不保存整表**，只保存行数，滚到哪一行
才向适配器要那一行。可视行由固定数量的行容器承载，滚动时换绑行号，
对象与字符串容量全程复用。

```cpp
#include "AiUI/widgets/list_view.h"

list->setColumns({{L"名称", 320, ListAlign::kLeft, true},
                  {L"类型", 110},
                  {L"大小", 100, ListAlign::kRight}});

auto source = std::make_shared<ListRowSource>();
source->setCount([&] { return static_cast<int>(items.size()); });
source->setBinder([](int row, ListRow& out) {
  out.tag = static_cast<int>(row);
  out.cells = {{items[row].name}, {items[row].type}, {items[row].size}};
});
list->setAdapter(source);
```

## 适配器

### ListAdapter

最小接口，三个方法：

```cpp
class ListAdapter {
  virtual int rowCount() const = 0;
  virtual void bindRow(int row, ListRow& out) const = 0;
  virtual int tagAt(int row) const;  // 默认返回行号本身
};
```

`bindRow` 的语义是"把第 `row` 行摊平到 `out`"。`out` 是复用的行容器，
进来的 `cells` 容量已经是上一行的，**直接赋值不要 reserve**。

### ListRowSource

函数式实现，不用写类：

```cpp
auto source = std::make_shared<ListRowSource>();
source->setCount([] { return count; });          // 行数
source->setBinder([](int row, ListRow& out) {}); // 某行怎么画
source->setTagAt([](int row) { return ids[row]; }); // 业务标识，可省略
```

### ListArrayAdapter

数据本来就在库这边时最省事：

```cpp
auto adapter = std::make_shared<ListArrayAdapter>(std::move(rows));
list->setAdapter(adapter);
```

`bindRow` 会就地覆盖目标，保留已有的字符串容量。

## ListRow

| 字段 | 说明 |
| --- | --- |
| `cells` | 每列的文本与配色，下标对应列 |
| `icon` | 行首图标，只画在第 0 列 |
| `tag` | 业务标识，库不解释，原样回传 |
| `dimmed` | 整行禁用外观（浅灰字） |

`cells` 少于列数时，缺的列不画；`cell.text` 为空也不画。

## 列定义

```cpp
struct ListColumn {
  std::wstring title;
  int width = 120;                 // 96 DPI 设计值
  ListAlign align = ListAlign::kLeft;
  bool stretch = false;            // 吃掉剩余宽度
};
```

通常只有第一列（名称）设 `stretch = true`。拉伸列被用户拖过宽度之后，
就不再自动铺满，尊重用户设置。

## 行容器复用

这是长列表不卡的关键。滚动时：

1. 计算出本次可视的行号区间 `[first, last]`。
2. 池子按可视行数封顶（`pageHeight / rowHeight + 2`），不随总行数增长。
3. 滚出区间的行容器标记为空闲，`row = -1`。
4. 新进区间的行取一个空闲容器，绑定新行号。

容器数量恒定为一屏，2 万行的目录也只占十来个 `ListRow` 的内存。

绑定时有短路：行号和版本都没变就跳过 `bindRow` 调用。所以同一行反复
重绘（悬停、选中）不会重复取数。

## 数据变更

| 方法 | 场景 |
| --- | --- |
| `notifyDataSetChanged()` | 行数或顺序变了；清空选中，全部重绑 |
| `notifyRowsChanged(first, last)` | 只有这几行内容变了；保持选中，只重绑这几行 |

两者都只重绘受影响区域，不重建控件。

```cpp
// 排序、换目录、过滤：行数和顺序都变了
SortItems();
list->notifyDataSetChanged();

// 某个文件大小变了：只影响一行
list->notifyRowsChanged(row, row);
```

`notifyDataSetChanged` 会清空选中，因为行号已经不可信。如果需要保持选中，
按业务标识（`tag`）自己恢复。

## 选择

| 方法 | 说明 |
| --- | --- |
| `selectedRows()` | 所有选中行号 |
| `selectedCount()` | 选中数量 |
| `selectRow(int, bool scrollIntoView = true)` | 单选某行并清掉其它 |
| `selectAll()` | 全选 |
| `clearSelection()` | 清空 |
| `focusedRow()` | 当前焦点行（锚点） |

交互规则与资源管理器一致：

| 操作 | 效果 |
| --- | --- |
| 单击 | 单选 |
| `Ctrl + 单击` | 切换该行选中 |
| `Shift + 单击` | 从锚点连选到该行 |
| `Ctrl + A` | 全选 |
| `↑` / `↓` / `Home` / `End` / `PageUp` / `PageDown` | 移动选中并滚入视野 |

## 排序与列宽

```cpp
list->setSort(column, ascending);        // 设置排序指示箭头
list->sortColumn(); list->sortAscending(); // 读取当前状态
list->setUserWidth(column, physicalPx);  // 恢复持久化的列宽
list->columnWidth(column);               // 读当前列宽（物理像素）
```

点表头触发 `onSort(column, ascending)`，同列重复点击切换方向；拖列边界
触发 `onColumnResized`。控件只画箭头和调整宽度，**不自己排序**——排序是
业务侧的事，改完数据调 `notifyDataSetChanged()`。

`setUserWidth` 接收的是物理像素，持久化再恢复时不要再乘一次 DPI。

## 回调

| 回调 | 触发时机 |
| --- | --- |
| `onActivate(int row)` | 双击行或按回车 |
| `onSelectionChanged()` | 选中发生变化 |
| `onSort(int column, bool ascending)` | 点表头 |
| `onContextMenu()` | 右键 |
| `onKeyDown(Key)` | 未消费的按键（Delete、Backspace 等） |
| `onColumnResized()` | 拖完列宽松手 |

`onKeyDown` 只收到列表自己不处理的键；方向键、回车、`Ctrl+A` 已被内部消费。

## ListStyle

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `headHeight` / `rowHeight` | 40 / 44 | 表头与行高 |
| `padX` | 16 | 单元格左右内边距 |
| `iconSize` / `iconGap` | 18 / 8 | 行首图标尺寸与间距 |
| `selBg` / `hoverBg` | 浅蓝 / 浅灰 | 选中、悬停底色 |
| `text` / `headText` | — | 正文、表头文字色 |

## 局部重绘

| 变化 | 重绘范围 |
| --- | --- |
| 悬停进出 | 仅进出两行 |
| 选中变化 | 仅行区（不动表头） |
| 滚动 | 仅行区 |
| 拖列宽 | 整控件（列边界影响所有行） |

## XML

```xml
<ListView id="list" weight="1" height="match"/>
```

列、适配器、回调都要在代码里配。

## 注意

- 继承自 `ScrollView`：内容超出可视高度就自动出现自绘滚动条，不需要额外开关。
- 滚动条只画在表头下方的行区内，不会盖住表头。
- 需要双击消息，窗口类注册时已带 `CS_DBLCLKS`。
- `ListIcon::image` 有效时优先于 `glyph`；失效时退回矢量图标。
- `setAdapter` 会重置滚动位置与选中，换热数据源时才用。
- 列表自己不排序、不过滤，只按行号取数。
