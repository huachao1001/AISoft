# AiZip

压缩 / 解压工具应用。界面用 `AiUI` 自绘，内核是官方 7-Zip 静态库。

## 功能

- **浏览**：双击 exe 打开文件列表；命令行传目录或压缩包直接定位
  `AiZip.exe <路径>`
- **添加**：把选中项（未选中则当前目录）打包，弹出压缩对话框
- **解压到**：解压选中项或整个压缩包
- **删除**：移入回收站
- 地址栏可编辑（`Ctrl+L`），回车跳转；右侧搜索框（`Ctrl+F`）按名称过滤
- 双击压缩包进入浏览模式，双击目录进入下一层，双击文件直接解压
- 支持拖放：拖入压缩包直接浏览，拖入其他文件弹出压缩对话框
- 单实例：重复启动会把参数转发给已运行窗口

压缩格式：7Z / ZIP / TAR / GZ
解压格式：全格式（zip、7z、rar、tar、gz、bz2、xz、cab、iso、wim、lzh、
arj、001 分卷等），含 7z 头部加密。

## 目录

```
src/
  main.cpp          程序入口：单实例、DPI、命令行分发
  app.cpp           视图状态：当前目录、压缩包、导航、刷新
  archive.cpp       7-Zip 封装（打开、列出、解压、压缩）
  sevenzip_loader.cpp  静态 7-Zip 内核的格式与算法信息
  ui_main.cpp       主窗口：由生成的 MainFrame 布局装配
  ui_crumb.cpp      面包屑导航条（自绘 Widget，含搜索框）
  ui_toolbar.cpp    工具栏五个按钮与可用性
  ui_tree.cpp       左侧文件夹树
  ui_list.cpp       文件列表：列、排序、右键菜单、键盘
  ui_status.cpp     底部状态栏
  dlg_util.cpp      对话框辅助：窗口类、居中、模态循环、原生控件
  dlg_compress.cpp  压缩对话框
  dlg_extract.cpp   解压对话框
  dlg_input.cpp     轻量输入框
  dlg_progress.cpp  进度对话框（工作线程）
  shell_menu.cpp    资源管理器右键菜单注册
  single_instance.cpp
  util.cpp          路径、DPI、尺寸格式化、设置持久化
  res/              图标与 comctl32 v6 清单
assets/layouts/     XML 界面布局（构建期自动转 C++）
third_party/7z/     7-Zip 26.05 源码（与官方源码包同构）
tools/              内核验证程序 test_arc 与清单生成脚本
```

## 界面装配

主窗口本身只做原生宿主（窗口类、拖放、标题、窗口位置持久化），内部界面
全部来自 XML 布局：

```xml
<!-- assets/layouts/main_frame.xml -->
<LinearLayout orientation="vertical">
  <CrumbBar id="crumb" ns="ui" header="ui_crumb.h" height="48"/>
  <ButtonBar id="toolBar" height="52"/>
  <LinearLayout id="content" orientation="horizontal" weight="1">
    <TreeView id="tree" width="230" height="match"/>
    <ListView id="list" weight="1" height="match"/>
  </LinearLayout>
  <StatusBar id="status" height="36"/>
</LinearLayout>
```

`main_frame.xml` 在构建期由 `aiui_add_layouts` 生成 `AiUI::generated::MainFrame`，
`WM_SIZE` 直接交给布局系统，不再手写 `MoveWindow`。各控件的行为在
`ui_toolbar.cpp` / `ui_tree.cpp` / `ui_list.cpp` / `ui_status.cpp` 里用回调装配。

## 构建

```bat
call "D:\vs2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S AiZip -B AiZip\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build AiZip\build --parallel
```

产物是单个 `AiZip\build\AiZip.exe`，CRT 已静态链接，目标机无需装 VC++ 运行库。

`AiZip` 通过 `add_subdirectory` 引入同级 `AiUI`，`AiUI` 不反向依赖本工程。

## 单文件是怎么做到的

`third_party/7z/build-cmake` 把官方 7-Zip 26.05 源码编成静态库 `7zstatic`，
直接链进 exe。构建清单取自官方 `7z.dll` 的配方
`CPP/7zip/Bundles/Format7zF/Arc.mak`，所以格式与算法支持和 7z.dll 一致。

与原版 dll 构建的必要差异：

- 不定义 `Z7_EXTERNAL_CODECS`：静态链接下没有外部 codec 宿主
- 入口换成 `Z7StaticExports.cpp`，去掉 `DllMain`
- 链接时加 `/WHOLEARCHIVE:7zstatic`：各格式靠 `*Register.cpp` 的全局对象在
  静态初始化阶段注册，这些对象没有外部引用，普通链接会被整体丢弃，
  结果是格式列表为空

清单由 `tools/gen_sources.ps1` 从 `Arc.mak` 解析生成（225 cpp / 46 c / 8 asm）。
汇编模块（CRC / AES / SHA / LZMA 解码加速）用 `ml64` 编译；配置期会把 `.asm`
里的共享宏 include 改写成绝对路径，因为 MASM 按工作目录而非源文件目录解析
相对路径。

## 精简范围

剔除：广告 SDK、首页推广、皮肤下载、升级检查、反馈上报、工具箱（批量改名 /
MD5 / 虚拟光驱）、图片视频 PDF 压缩与预览、云盘、分享、安全扫描、守护进程。

未实现：自解压（SFX）生成、压缩包注释写入、压缩包内直接增删成员。

技术选型与验证记录见 [DESIGN.md](DESIGN.md)。
