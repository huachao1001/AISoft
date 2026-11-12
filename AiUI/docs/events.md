# 事件分发

点击、滚轮这些事件**不是直接发给命中的控件**，而是按 Android 的三段式在
视图树上走一遍：

```
捕获 capture ──▶ 目标 target ──▶ 冒泡 bubble
根 → … → 父          命中视图        父 → … → 根
```

`ViewRoot` 负责命中测试，`EventDispatcher` 负责排列接收顺序。分发的实现放在
`src/events/`，本身不碰任何平台接口，可以脱离窗口单测。

## 三个阶段各做什么

### 1. 捕获：容器抢手势

从根往目标下沉，每层调用 `onInterceptMouseDown()`。返回 `true` 表示拦下：

- 目标与内层**完全收不到**该事件；
- 拦下者自己立即收到一次 `onMouseDown()`；
- 后续 `up` / `move` 也回到拦下者，不会还给原来的叶子。

典型用途是滚动容器：判断这次拖动是滚内容还是交给子控件。默认返回 `false`，
所以普通容器不需要关心这一层。

### 2. 目标：命中视图处理

命中的视图收到 `onMouseDown()`。返回 `true` 表示已消费。

### 3. 冒泡：向上给容器机会

没被消费就交给父级，逐层向上。任一层返回 `true` 或调用
`stopPropagation()`，冒泡立即停止。

## 停止传播

`MouseEvent` 自带 `propagation` 指针，由分发器注入：

```cpp
bool MyView::onMouseDown(const MouseEvent& e) override {
  if (!insideHotArea(e.position)) return false;  // 不消费，继续冒泡
  e.stopPropagation();                           // 消费并阻止外传
  return true;
}
```

- `stopPropagation()`：消费 + 阻止继续传（等同 Android 返回 `true`）
- `markHandled()`：只标记已处理，仍允许继续冒泡

`onMouseUp` / `onMouseMove` / `onDoubleClick` / `onContextMenu` 没有返回值，
消费只能由 `stopPropagation()` 表达——这跟 Android 的「不发回去就不再上报」
是同一种语义。

## 坐标

每一层收到的 `MouseEvent::position` 都是**该层自己的局部坐标**，不需要自己
换算。分发器用 `View::windowToLocal()` 逐层转换。

## 滚轮

`onMouseWheel` 没有捕获阶段：它不像拖动那样需要容器和子控件争抢手势。
仍然是「目标优先，未消费则冒泡」，所以内层滚不动时外层滚动容器会接手。

## 别在控件里手工转发

控件只实现自己的 `onMouseDown` / `onInterceptMouseDown`，不要自己调父级的
处理函数。所有顺序与中断都由分发器统一控制，否则会出现父子都处理一次、
或者 `up` 找不到按下者的问题。
