# TreeView

懒加载树。内部把树拍平成扁平行来渲染，所以展开一万个节点也只是一次
线性绘制。子节点由调用方按需提供。

```cpp
#include "AiUI/widgets/tree_view.h"

auto root = std::make_unique<TreeNode>();
root->label = L"";
root->isRoot = true;

tree->setRoot(std::move(root));
tree->setChildrenLoader([](TreeNode* node) {
  std::vector<std::unique_ptr<TreeNode>> children;
  for (const auto& dir : ListSubDirs(node->path)) {
    auto child = std::make_unique<TreeNode>();
    child->label = dir.name;
    child->path = dir.fullPath;   // 业务标识
    children.push_back(std::move(child));
  }
  return children;
});
tree->setSelectHandler([](TreeNode* node) { NavigateTo(node->path); });
```

## TreeNode

| 字段 | 说明 |
| --- | --- |
| `label` | 显示文字 |
| `path` | 业务标识，用于 `selectPath` 匹配与层级判断 |
| `depth` | 缩进层级，由控件维护 |
| `expanded` | 是否展开 |
| `loaded` | 子节点是否已加载过 |
| `hasChildren` | 是否有子节点（决定点击是否触发展开） |
| `isRoot` | 根节点标记，本身不渲染 |
| `icon` | 节点图标，默认文件夹 |
| `parent` | 父节点 |
| `children` | 子节点，`unique_ptr` 独占 |

`path` 的层级匹配按分隔符边界判断：`a` 不会误配 `a2`，但会匹配 `a\b`。
分隔符 `\` 和 `/` 都认。

## 懒加载

`setChildrenLoader` 传入的 loader 只在节点**第一次展开**时调用一次，
结果缓存在 `node->children` 里。之后展开不再查询。

loader 返回空数组表示"这个节点没有子节点"，控件会把 `hasChildren` 置
false，之后点击只做选中，不再尝试加载。

## 方法

| 方法 | 说明 |
| --- | --- |
| `setRoot(std::unique_ptr<TreeNode>)` | 换整棵树的根 |
| `setChildrenLoader(loader)` | 设置子节点加载器 |
| `setSelectHandler(fn)` | 选中回调 |
| `setStyle(const TreeStyle&)` | 覆盖视觉参数 |
| `rebuild()` | 重建可见行；根节点或展开状态变化后调用 |
| `selectPath(const std::wstring&)` | 展开到指定 `path` 并选中 |
| `active()` / `activePath()` | 当前选中节点 / 其 path |
| `collapseActive()` | 折叠当前节点，返回是否成功 |

`selectPath` 是幂等的导航入口：它会沿着 `path` 逐层展开并加载，适合在
外部状态变化后同步树的位置，不需要自己算该展开哪些层。

## 交互

| 操作 | 效果 |
| --- | --- |
| 单击节点 | 选中；若未展开则展开并加载 |
| 单击已展开的当前节点 | 折叠 |
| 悬停 | 整行浅灰底 |

树没有展开箭头。展开/折叠完全由点击节点本身触发，视觉上更干净。

## 局部重绘

| 变化 | 重绘范围 |
| --- | --- |
| 悬停进出 | 仅进出两行 |
| 选中变化（无展开） | 仅新旧两行 |
| 展开 / 折叠 / `selectPath` | 整控件（可见行集合变了） |

## TreeStyle

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `rowHeight` | 30 | 行高 |
| `indent` | 18 | 每层缩进 |
| `padLeft` | 14 | 左侧内边距 |
| `iconSize` / `iconGap` | 16 / 6 | 图标尺寸与间距 |
| `pillInsetX/Y` / `pillRadius` | 6 / 2 / 5 | 选中态圆角胶囊 |
| `selBg` / `hoverBg` | 浅蓝 / 浅灰 | 选中、悬停底色 |

## XML

```xml
<TreeView id="tree" width="230" height="match"/>
```

## 注意

- 继承自 `ScrollView`：内容超出可视高度就自动出现自绘滚动条。
- `setRoot` 会自动重建可见行并重置选中与滚动位置。
- 节点里的 `children` 由 `unique_ptr` 独占，重建子树时整枝替换即可。
- 树自己不排序，「文件夹在前」这类规则由 loader 决定顺序。
- `selectPath` 只做展开与选中，**不触发** `setSelectHandler`；这是为了让
  程序化同步不引起导航回调循环。
