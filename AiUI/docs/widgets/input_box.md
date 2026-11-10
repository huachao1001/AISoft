# InputBox

圆角输入框。文本、光标、选区、IME 组合串全部自绘，不创建 `EDIT` 子控件。

```cpp
#include "AiUI/widgets/input_box.h"

InputSpec spec;
spec.cue = L"搜索文件或文件夹";
spec.hasLeadIcon = true;
spec.leadIcon = IconId::kSearch;

auto* box = new InputBox();
box->setSpec(spec);
box->setOnChange([] { /* 内容变化 */ });
box->setOnSubmit([](bool committed) {
  if (!committed) return;  // Esc 取消
  DoSearch();
});
```

## InputSpec

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `cue` | 空 | 空内容时的提示文字 |
| `hasLeadIcon` | `false` | 是否画前置图标 |
| `leadIcon` | `kSearch` | 前置图标 |
| `leadIconSize` / `leadIconGap` | 14 / 5 | 图标尺寸与右间距 |
| `padX` | 11 | 左右内边距 |
| `radius` | 7 | 圆角半径 |
| `outerBackground` | `kColorPane` | 圆角外的四角填充色 |

`outerBackground` 必须和宿主底色一致，否则圆角外侧的四个小三角会露出
不协调的白色。放在面包屑里就取面包屑底色，放在对话框里就取对话框底色。

## 内容

| 方法 | 说明 |
| --- | --- |
| `text()` | 当前文本 |
| `setText(const std::wstring&)` | 替换文本，光标移到末尾 |
| `setCue(const std::wstring&)` | 改提示文字 |
| `selectAll()` | 全选 |

`text()` 返回的是灰态（cue）之外的实时内容；空内容时返回空串，不是 `cue`。

## 回调

| 回调 | 触发时机 |
| --- | --- |
| `setOnChange(fn)` | 文本变化，每次输入/删除都触发 |
| `setOnSubmit(fn)` | 回车（`committed=true`）或 Esc（`committed=false`） |
| `setOnBlur(fn)` | 失去焦点 |

Esc 也走 `onSubmit`，靠 `committed` 参数区分。这个设计是为了让调用方
在一处收口"编辑结束"。

## 键盘支持

| 按键 | 行为 |
| --- | --- |
| 字符 | 插入到光标处，替换选区 |
| `Backspace` / `Delete` | 删除前/后一个字符，有选区时删选区 |
| `Left` / `Right` | 移动光标 |
| `Ctrl+Left` / `Ctrl+Right` | 按词移动 |
| `Home` / `End` | 移到行首 / 行尾 |
| `Shift + 方向键` | 扩展选区 |
| `Ctrl+A` | 全选 |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | 复制 / 剪切 / 粘贴 |
| `Enter` | `onSubmit(true)` |
| `Esc` | `onSubmit(false)` |

IME 组合串通过 `onCompositionStart/Update/End` 处理，中文输入法下未上屏的
拼音会画成带下划线的组合串，不会误触发 `onSubmit`。

## 焦点与光标

- 获得焦点后光标闪烁，失焦立即停止。
- 点击框内任意位置会按字符位置命中，把光标放过去。
- 双击选中一个词。
- 拖拽可以拉选区。
- 内容超宽时自动横向滚动，保证光标始终可见。

## 测量

`onMeasure` 返回固定高度 `34 * dpi / 96`，宽度返回 0 交给父容器决定。
所以 `InputBox` 通常要显式给宽度，或者在 `LinearLayout` 里用 `weight`。
高度也可以由 `LayoutParams` 覆盖。

## XML

```xml
<InputBox id="search" cue="搜索" icon="search" radius="7" padding="11" outerColor="#ffffff"/>
```

| 属性 | 说明 |
| --- | --- |
| `cue` | 提示文字 |
| `icon` | 前置图标名，写了就自动开 `hasLeadIcon` |
| `radius` | 圆角半径 |
| `padding` | 左右内边距，对应 `spec.padX` |
| `outerColor` | 圆角外填充色，对应 `spec.outerBackground` |

## 注意

- `setText` 不会触发 `onChange`，避免程序化赋值引起回调风暴。
- 输入框自己不画边框颜色以外的装饰，聚焦态边框由内部根据焦点状态切换。
- 单行控件，不支持换行；需要多行文本请用 `TextView` 或自绘。
