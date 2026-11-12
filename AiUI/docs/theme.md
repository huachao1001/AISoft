# 主题

配色有两层：**运行时可切换的 `Theme` 对象**，以及构建期由 `themes.xml`
编译出的命名调色板。控件不写死颜色，绘制时向所在视图树取当前主题，
所以换主题不需要重建控件，也不会漏掉某个控件。

## 为什么不是常量表

`theme.h` 里确实还有一组 `kColor*` 常量，但它们是历史遗留，取值等同浅色
主题。控件内部一律用 `theme()` 取色：常量无法换肤，也无法让同一进程里
两个窗口用不同配色。

## 基本用法

```cpp
// 全局默认：所有 Activity 创建时取它，对话框这类没有单独指定主题的界面
// 都靠这一句跟上
AiUI::Activity::setDefaultTheme(&AiUI::kDarkTheme);

// 单个界面覆盖全局默认
class MyActivity : public AiUI::Activity {
  // 构造函数里设一次即可，创建窗口时自动应用
};
```

窗口层也能直接设：

```cpp
window.setAppTheme(&AiUI::kDarkTheme);
window.setDarkMode(true);          // 便捷入口，等价于上面
```

`Theme` 对象的生命周期必须覆盖窗口。用 `kLightTheme` / `kDarkTheme`
这类静态量，或者 `ThemeRegistry` 里登记的对象。

## 视图树内继承

主题沿父链继承，根上设一次就覆盖整棵树：

```cpp
root->setTheme(&AiUI::kDarkTheme);
// 后代全部跟随，包括之后加进来的子视图
leaf->theme().body;                 // → kDarkTheme.body
```

子树可以单独覆盖，做局部换色：

```cpp
sidebar->setTheme(&AiUI::kLightTheme);   // 只有这棵子树变浅
sidebar->setTheme(nullptr);              // 恢复继承
```

`theme()` 的查找顺序：自身 → 祖先 → 浅色主题。

## 用 themes.xml 配置

`themes.xml` 在构建期被编译成一组静态 `Theme`，登记进 `ThemeRegistry`，
运行时按名字取：

```xml
<themes>
  <Theme name="light"/>

  <Theme name="dark"
         body="#1e1e1e"
         pane="#252526"
         text="#d4d4d4"
         textFaint="#6a6a6a"
         link="#4a9eff"
         inputBg="#1e1e1e"
         titleBg="#202020"
         titleLine="#3c3c3c"/>
</themes>
```

```cmake
aiui_add_themes(AiZip assets/layouts/themes.xml)
```

```cpp
const AiUI::Theme& theme =
    AiUI::ThemeRegistry::getOr(g_app.settings.theme, AiUI::kLightTheme);
window.setAppTheme(&theme);
```

**只写要改的字段**，其余沿用浅色主题的默认值。属性名与 `Theme` 的字段
同名，完整清单见 `AiUI/theme.h`。

## XML 里引用主题色

控件的颜色属性写 `#rrggbb` 是固定色，写主题字段名则随主题切换：

```xml
<!-- 固定色：换主题不跟着变 -->
<TextView text="提示" textColor="#555555"/>

<!-- 跟随主题：深色主题下自动变成浅灰 -->
<TextView text="提示" textColor="textHead"/>

<LinearLayout background="dialogBg">
  <ProgressBar fillColor="link" trackColor="lineSoft"/>
</LinearLayout>
```

能在 XML 里写主题名的属性：

| 属性 | 作用 |
| --- | --- |
| `background` | 任意节点的底色 |
| `textColor` | 文字色 |
| `borderColor` / `focusBorderColor` | 描边与聚焦描边 |
| `backgroundColor` | 输入类、按钮类控件的底色 |
| `fillColor` / `trackColor` | 进度条 |
| `boxColor` / `accentColor` | 复选/单选框 |
| `arrowColor` / `iconColor` / `caretColor` | 图标与插入点 |

可用的名字就是 `Theme` 的字段：`body`、`tool`、`pane`、`line`、`text`、
`textHead`、`textMuted`、`textFaint`、`ink`、`link`、`selBg`、`inputBg`、
`inputFocus`、`dialogBg`、`titleBg`……写错名字会退回控件默认值。

## 派生色：描边不要写死

描边色一律由底色派生，不要配一组固定灰：

```cpp
painter.strokeRoundRect(rect, edgeOn(background, 17), radius);
```

`edgeOn` 按底色的亮度选墨色——浅底叠黑、深底叠白——再按百分比混合。
写死 `#d1d5db` 在白底上正常，到了深色主题上就是一根刺眼的白线。

`spec.border` 这类显式描边仍可覆盖派生值，只在确实需要精确控制时用。

## 深色主题下的注意事项

- **不要在应用代码里写死颜色**。`style.background = kColorTool` 会让控件
  脱离主题，深色下留一块白斑；留空即可跟随。
- 图片资源要准备深色版本，或者用矢量图标并按 `Theme::iconMuted` 上色。
- 半透明色在深底上会更淡，禁用态浓度要按主题调，不能照搬浅色的档位。
