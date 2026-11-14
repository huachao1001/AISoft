# AiInstaller

通用安装器：**安装器本体固定不变，打包时把清单与文件追加到它末尾**。同一个
`AiSetup.exe` 能安装任意应用——换应用只换清单与载荷，不重新编译安装器。

```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../AiInstaller ${CMAKE_CURRENT_BINARY_DIR}/AiInstaller)

aiinstaller_add_package(
  INSTALLER AiSetup                       # 通用安装器目标
  MANIFEST  "${CMAKE_CURRENT_SOURCE_DIR}/setup.ini"
  ROOT      "${CMAKE_CURRENT_BINARY_DIR}/setup_root"   # 清单里的文件相对这里
  OUTPUT    "${CMAKE_CURRENT_BINARY_DIR}/MyAppSetup.exe"
  ICON      "${CMAKE_CURRENT_SOURCE_DIR}/src/res/myapp.ico"
  DEPENDS   MyApp my_config
)
```

应用侧不写一行安装代码，只需要一份 `setup.ini` 和竖好的 `ROOT` 目录。

## 产物结构

```
MyAppSetup.exe
├── AiSetup.exe（通用安装器本体）
├── 清单（UTF-8 INI）
├── 载荷（清单里登记的文件，顺序拼接）
└── Trailer（32 字节，末尾倒查魔数 AISETUP1）
```

布局契约在 `include/AiInstaller/package.h`，打包器 `tools/pack/pack_main.cpp`
与读取端 `src/package.cpp` 必须一致。**顺序不能反**：读取端按
`exe → 清单 → 载荷` 解释，且 `UpdateResource` 会重排 PE 映像，所以打包必须
「先改资源（图标、版本），再追加清单与载荷」。

## 清单长什么样

INI 文本，分五段。完整示例见 `AiZip/setup.ini`。

```ini
[app]
displayName=我的应用          ; 界面与卸载列表里的名字
version=1.0.0
publisher=我的公司
exeName=MyApp.exe             ; 主程序，[run] 与配置模块都以它为基准
installSubdir=AISoft\MyApp    ; 只写子目录，盘符由安装器在运行期挑

[ui]                          ; 界面只有一个通用模板，全靠这些文案与 logo 品牌化
windowTitle=安装我的应用
description=……
logo=logo.png                 ; 随包携带，不装到用户目录
showScope=1                   ; 是否显示「所有用户 / 仅当前用户」
runAfterText=立即打开          ; 完成页复选框的文案
runAfterChecked=1             ; 复选框默认是否勾上

[config]
module=my_config.dll          ; 按标准 ABI 导出的配置 dll，可留空

[commands]                    ; 安装后按顺序执行的命令，text 显示在进度条下方
cmd.0.path=reg.exe            ; 裸文件名走 PATH；相对路径相对安装目录
cmd.0.args=add "${root}\Software\Classes\MyApp.Archive" /ve /d "说明" /f
cmd.0.text=正在登记文件类型说明...
cmd.0.required=0              ; 失败是否影响安装结果，默认 1

[run]
afterInstall=MyApp.exe        ; 「立即打开」勾上时才执行，留空则隐藏复选框

[files]
file.0.path=MyApp.exe         ; 相对 ROOT；offset/size 由打包器补
file.1.path=my_config.dll
file.2.path=logo.png
file.2.install=0              ; 只随包携带，不落盘
```

命令的 `path` / `args` 里可用两个占位符，由安装器按本次安装展开：

| 占位符 | 展开成 |
|---|---|
| `${dir}` | 安装目录（不含尾部分隔符） |
| `${root}` | 注册表根键：所有用户 → `HKLM`，仅当前用户 → `HKCU` |

**必须用 `${root}`，不要写死 `HKLM`**：选「仅当前用户」或静默安装时写 HKLM
是越权，而卸载只清 HKCU，留下的项没人收。

## 它负责什么

| 能力 | 说明 |
|---|---|
| 界面 | 三段式（配置 → 进度 → 完成），一个通用模板，全部由清单驱动 |
| 进度 | 按「文件数 + 配置 + 命令数 + 收尾步骤」真实计步，进度条下方实时显示当前动作 |
| 默认安装路径 | 默认跳过系统盘：按盘符字母序找第一个**可写**的固定盘，非系统盘全部不可写时才用系统盘。子目录由清单给（不含盘符） |
| 落盘 | 按清单 offset/size 从自身载荷解压，分块复制，允许子目录 |
| 命令 | 按清单 `[commands]` 顺序执行，支持 `${dir}` / `${root}` 占位符 |
| 快捷方式 | 开始菜单 `.lnk` |
| 卸载程序 | 安装目录里生成 `uninstall.exe`（安装器本体的裁剪副本），「应用与功能」里的「卸载」指向它 |
| 卸载 | 先撤销配置，再删清单登记的文件，最后删空目录与安装器新建的父目录 |
| 提权 | `--elevated-retry` 参数往返，用户只点一次 UAC |
| 静默安装 | `--silent`，默认仅当前用户（无人可点 UAC） |

安装跑在工作线程上：`runInstall()` 会同步复制几十 MB 并等命令返回，占满 UI
线程期间进度条一格都不会动。进度回调只写快照，UI 线程用 60ms 定时器取走，
避免每条进度都投递窗口消息灌满队列。

## 它不负责什么

**应用私有的系统配置**——右键菜单、文件类型关联、开机自启——由应用的配置
dll 按标准 ABI 导出（`include/AiInstaller/config_abi.h`）：

```c
AiSetupConfigResult AiSetupConfigApply(const AiSetupConfigRequest* request);
AiSetupConfigResult AiSetupConfigRemove(const AiSetupConfigRequest* request);
int AiSetupConfigIsReady(const AiSetupConfigRequest* request);
int AiSetupConfigVersion(void);
```

安装器只按这套约定 `LoadLibrary` + `GetProcAddress`，不认识任何应用私有的
导出名。同一个 dll 也被应用主程序调用来自愈（菜单丢了、程序换了位置）。

## 命令行

| 参数 | 含义 |
|---|---|
| （无） | 交互式安装 |
| `--silent` | 不建界面，装到默认目录（仅当前用户） |
| `--uninstall` | 卸载，安装目录即本程序所在目录（`uninstall.exe` 用它） |
| `--elevated-retry …` | 提权后的新进程，按带过来的目录与范围直接装 |

## 安装目录里有什么

```
<安装目录>\
├── MyApp.exe              清单里 install=1 的文件
├── my_config.dll
├── uninstall.exe          安装器本体的裁剪副本，卸载入口
├── installer.manifest     清单原文，卸载据此读回文件表
├── installer.scope        安装范围（all-users / current-user）
└── installer.dirs         安装器当年新建的目录层级，卸载据此收回父目录
```

`uninstall.exe` 不另编一个小程序，直接是安装器本体换个名字：它已经带好了应用
图标与版本资源，也自带 `--uninstall` 通道。裁剪到 `dataOffset` 之后它不含载荷，
卸载改从 `installer.manifest` 读文件表。名字固定成 `uninstall.exe` 而不是安装包
的名字（`AiZipSetup.exe`）——卸载入口叫「卸载」才符合用户预期。

## 卸载为什么读 installer.manifest

装进安装目录的安装器副本被裁到 `dataOffset`（不含载荷），它打不开自己末尾的
包。所以安装时把清单另存成 `installer.manifest`，卸载从那里读文件表与配置
模块。

## 卸载要收回哪些目录

安装时用 `missingDirectories()` 记下「这条路径上哪几级目录现在还不存在」，
写进 `installer.dirs`；卸载按记录由内到外 `rd` 收回。**不按目录名猜系统目录**：
每一级都是安装器实际创建的，`Programs`、`Users` 这些早就存在的目录根本不会
进记录。

收尾进程只用 `rd`（不带 `/s`）、只 `del` 安装目录里的 `uninstall.exe`：目录里
用户自己放的东西必须留着。带子目录的清单文件（如 `config\x.dll`）删完会留下空
子目录，卸载时按文件表推导出来一并清掉，否则安装目录永远非空。

## 目录约定

```
include/AiInstaller/   公开接口：清单、包、安装编排、配置 ABI（无平台类型）
src/                   通用实现：清单解析、包读取、路径策略、安装/卸载
src/ui/                通用界面（布局在 assets/layouts/dlg_setup.xml）
src/os/<platform>/     平台差异（目前只有 Windows）
tools/pack/            打包器 aiinstaller_pack
cmake/                 构建期辅助函数
```

「默认路径不含盘符」是刻意的：装到哪个盘是本机事实，由策略在运行期决定，
清单只声明子目录（如 `AISoft\MyApp`）。

选盘规则：**默认优先非系统盘**，按盘符字母序取第一个能写的固定盘；非系统盘
全都不可写时才回退系统盘。盘存在不代表装得进去（有些机器只给管理员写盘根），
所以会先建一个探针文件确认可写性再选。两个安装范围共用同一套选盘与子目录，
范围只决定写 HKCU 还是 HKLM、要不要提权。
