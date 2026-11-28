# 架构决策：AOT 编译 + 分层运行时 + `.pylib` 扩展

> 状态：**已落地**（代码与测试都在仓库里，见每节末尾的验证入口）

---

## 1. 产品形态

用户只写两种文件：`.py` 与 `.xml`。其余一切由工具链负责。

| 层 | 形态 | 运行期是否存在 |
|---|---|---|
| 界面库 | `pyui` 静态库（自绘控件 + 平台后端） | 是 |
| 语言运行时 | AiPy 静态库（`Value` + 语言内建） | 是 |
| 业务脚本 | 构建期由 `pyuic` 编译成 C++ | **否**（已是机器码） |
| 布局资源 | 构建期由 `pyui_layoutc` 编译成 C++ | **否** |
| 扩展 | `.pylib` 包（zip），内含原生静态库 | 链接期消费，产物里无包解析器 |

产物里没有一个字节的 Python，也没有解释器。

---

## 2. 分层：语言层是**另一个项目**

语言运行时不是本仓库里的一个 target，而是同级的独立项目 `../AiPy`。
PyUI 用 `add_subdirectory` 消费它，方向严格单向：

```
        AiPy（../AiPy，可独立构建）         PyUI（本仓库）
        +----------------------+           +-------------------+
        | aipy_base  UTF-8 工具 | <---------| pyui_core         |
        | aipy       Value/内建 | <----+    | 控件/绘制/后端    |
        +----------------------+      |    +-------------------+
                                      |             ^
                            +---------+-------------+-------+
                            | pyui_script  桥接层           |
                            | 唯一同时知道 Value 与 View    |
                            +---------------+---------------+
                                            v
                                       应用 / 扩展
```

| 库 | 所在项目 | 职责 | 硬约束 |
|---|---|---|---|
| `aipy_base` | **AiPy** | `utf8Length` 等字符串工具 | 只依赖标准库 |
| `aipy` | **AiPy** | `Value`、算术/比较/容器、字符串内建、`min/max`、`parse_number`、字节码 VM 与编译器 | **零界面依赖**，可独立构建运行 |
| `pyui_core` | PyUI | 控件、绘制、布局、平台后端 | **零 Python 依赖**，不知道 `Value` 存在 |
| `pyui_script` | PyUI | `Script` 基类、控件门面（`setText`/`onClick`…）、事件与菜单编码 | 唯一可以整体替换的层 |

分出去的理由不是「少编一个文件」：两个项目的交付节奏不同。AiPy 要能被
命令行工具、测试、将来的服务端宿主单独拿走，而它们不该被迫带一个窗口系统。

三条边界不是口头约定，各有自动化守护：

- `tools/check_layers.py`：界面源文件不许直接 include `aipy/` 的头（只能走
  `pyui/` 的转发头）；PyUI 里不许再出现 `py_value.cpp` / `utf8.cpp` 的实现
  副本；`pyui/utf8.h`、`pyui/py_value.h` 必须是转发头而不是定义；CMake 必须
  确实通过 `add_subdirectory` 引入 AiPy；并反向扫描 AiPy，确认它里面没有任何
  界面符号。
- `AiPy/tests/test_standalone.cpp`：一个**只链 `aipy`** 的可执行程序。符号缺
  一个就链接失败，比读 CMakeLists 里的依赖边可靠——依赖边会被 `PUBLIC` 传递进来。
- 两者都是 CTest 用例（`pyui_layers`、`aipy_standalone`）。

真正强的证据在仓库外：把 `PyUI/` 整个目录改名藏起来，`AiPy/` 照样配置、编译、
跑完全部测试（含字节码端到端）。这才是「独立项目」，而不是「同一个 CMake
树里少声明一条边」。

这条实测抓到过一次真实退化：字节码编译器最初被放在 `PyUI/tools/pyuic/`，
AiPy 的构建期引用那个路径。一藏起 `PyUI`，`cmake` 就报
`'.../PyUI/tools/pyuic/vm_compiler.py' missing`——语言项目因为「编不了
自己的语言」而不能独立构建。编译器随即搬进 `AiPy/tools/aipyic/`。

历史教训一：`py_value.cpp` 曾经 `#include "pyui/painter.h"` 却没用到任何符号。
这类多余 include 是分层腐烂的起点，删掉它是净收益。

历史教训二：`Value` 曾直接存 `View*`（`kView` 类型），于是「语言层」在头文件里
就认识了控件。迁出时改成不透明的 `Host`（`void*`）：AiPy 原样存、原样带回，
把指针解释成 `View*` 是 `pyui_script` 的事——那正是这一层存在的理由。


---

## 3. 为什么交付走 AOT，开发期走字节码

早期原型做过字节码 VM。实测数据（`spike/`，可复跑）保留在这里，因为
「为什么要为它定点让步」比「选了它」更需要证据：

| 项目 | 实测 |
|---|---|
| CPython 300 万次累加 | 1010 ms |
| 直接 C++ 函数调用（下界） | 1.11 ms |
| VM，`Value` = 96 字节 | 805 ms |
| VM，`Value` = 16 字节 | 477 ms |
| 纯字节码分派（载荷是 `long long`） | 92 ms |
| 直调 `py::add`，不经解释器 | 154 ms |
| 单次扩展调用 | 94 ns |

第一轮把 805 ms 全归给「每次 push/pop 拷贝 96 字节」。瘦到 16 字节只到
477 ms——**假设错了**，如实记下来：剩下约一半开销在指令编码与栈管理。

这些数据决定的是**交付形态**，不是「要不要有 VM」：

1. **交付走 AOT**。脚本层是胶水，能力全在原生扩展里，一次点击回调慢 20 倍
   在 16 ms 帧预算里看不见；但运行期解释器有固定体积成本（解释器 + 字节码
   + 常量池），而 AOT 的成本在构建期。300–500 KB 的余量应该留给真正要做的事。
2. **开发期走字节码**。AOT 的代价是「改脚本要重编并重新链接整个 exe」。
   改一行界面代码等十几秒，反馈周期会把人赶走。字节码把这一步压到几十毫秒。
   界面脚本的瓶颈在重绘与 GDI，不在算术，477 ms 那个基准在这里不成立。
3. **AOT 的报错质量更高**：`pyuic` 有 AST 和行号，属性/参数/作用域错误在
   构建期就带 `file:line:col` 报出来。

两条路线共用同一套语言子集，所以「开发期能跑」和「交付后行为一致」是同一
份语义定义，不会出现「VM 里对、AOT 里错」。

字节码的 VM、编译器、加载器都在 `../AiPy` 里，不在本仓库：它们是「这门语言
怎么执行自己的程序」的一部分，和 `Value` 一样属于语言层的职责。

---

## 4. `Value` 是 16 字节（已落地）

```cpp
class Value {
  enum class Type : std::uint8_t { kNone, kBool, kInt, ... };
  Type type_;            // 1 字节（必须指定 uint8_t，默认 int 会撑到 4）
  union {                // 8 字节，四种载荷互斥
    long long int_;
    double float_;
    View* view_;
    HeapData* heap_;     // string / list / dict 统一走堆
  };
};
```

三个决定，每一个都是踩出来的：

1. **堆指针必须进 union**。`std::shared_ptr` 自己就 16 字节，当成员会让
   结构体回到 32 字节，瘦身等于白做。
2. **侵入式引用计数**（`HeapData::refs`），不用 `shared_ptr`：少一个控制块、
   少一次分配。非原子——脚本只在 UI 线程跑。
3. **`bool` 不单独存字段**，存成 `kInt` 的 0/1。踩过：`Value(true)` 写
   `int_`，`toBool()` 读 `bool_`，于是 `True` 变 `False`，基准里的循环
   一次都不进，跑出一个「0.02 ms」的漂亮数字。同一件事有两个表示，
   就一定有写一个忘另一个的那天。

`std::string` 的 32 字节必须放弃——字符数据一律堆分配。代价是短字符串多
一次分配，换来每次栈操作少拷 56 字节。

---

## 5. 扩展：`.pylib` 包

### 5.1 一个称呼只指一件事

早期 `.par` 这个称呼同时指「分发包」和「包里的静态库」，两件事共用一个词，
于是「`.par` 里的 `.pylib`」这种句子到处出现，读的人要一直猜。

现在固定为：

| 称呼 | 指什么 | 后缀 |
|---|---|---|
| **`.pylib`** | 可分发的**包**（zip，类比 Android AAR） | `.pylib` |
| 包内原生库 | 真实静态库，交给链接器 | Windows `.lib`、macOS/Linux `.a` |

包内库用平台原生后缀而不是统一改名，因为包解开后就是真实文件，没有
「消费方要靠扩展名猜编译器」的问题。而包与库共用同一后缀反而让
「哪个是包、哪个是库」永远需要靠上下文区分。

### 5.2 包结构

```
fs.pylib
├── manifest.json                  # 模块名、版本、函数表、平台 → 库路径
├── include/pyui/ext/fs.h          # 扩展的 C++ 头
└── lib/win-x64/fs.lib             # 静态库
```

`manifest.json` 里 `native.lib` 写的是**包内相对路径**，相对清单文件所在
目录解析。构建产物因此落在 `ext/<name>/lib/<platform>/`（源码树内），
而不是 `build/`——清单是分发格式，让它指向 `build/` 会在打包时立刻失效。

### 5.3 记录与链接分离

| 工具 | 语言 | 知道什么 | 产出 |
|---|---|---|---|
| `pyuic` | Python | AST、行号、`import` 语句 | `used.json`（用了哪些模块） |
| `pyui_extc` | C++ | 清单、依赖闭包、平台 | `app.ext.json` + `app.rsp` |

**为什么 `pyui_extc` 用 C++**：它要在用户机器上跑，那里只装链接器 kit、
没有 Python。`pyuic` 留在 Python 是因为前端必须是 CPython 的 `ast`——
换掉就失去了「语法覆盖等于 CPython」这个唯一优势。

**为什么是 C++ 而不是自写 JSON**：用 `nlohmann/json` v3.11.3（MIT，单头）。
解析器不属于这个产品的创新点，自写只会引入 bug。

清单 schema 只在 `tools/extc/manifest.cpp` 一处实现，`pyuic` 侧的
`extensions.py` 是读端，两者用同一份 `manifest.json`。

### 5.4 用了才链

粒度是「哪个 `.pylib`」，函数级裁剪交给链接器（`/Gy` + `/OPT:REF,ICF`）。

已验证：不 `import` 的脚本 `used.json` 为空、`app.rsp` 0 字节、生成代码里
零 `ext/` include。`calculator` 与 `progress` 两个示例就是活的证据。

### 5.5 分发出去的库必须关 `/GL`

`/GL` 的产物里是 MSIL，链接期要调 `c2.dll` 重新生成机器码。用户机器上
只有 `link.exe`，给 `.pylib` 里的库开 `/GL` 等于让它在别人机器上链接失败。

因此 AiPy 的 `aipy_base` / `aipy`、本仓库的 `pyui_core`、以及所有扩展库都显式
`/GL-`，只有可执行文件（示例、测试、工具）享受 `/GL` + `/LTCG`。

`dumpbin /headers` 验证：各静态库的 `.obj` 都是 `machine (x64)`，无 MSIL。

---

## 6. 体积

见 `DESIGN.md` 第 7 节，那里是**实测值**而不是估算：空 `wWinMain` 98 KB，
加窗口与平台后端到 385.5 KB，加控件只涨 1.5 KB。真实示例落在
416–474 KB，在 300–500 KB 目标内。

复现入口：`-DPYUI_BUILD_SIZE_PROBE=ON` 会生成四个逐层探针目标。

---

## 7. 顺带修掉的架构问题

### 7.1 UTF-8 工具从绘制层搬出来

`utf8Length` 等函数原本住在 `painter.cpp` 里，于是任何要处理字符串的层
都被拖进整个绘制层：脚本层算 `len("中文")`、扩展库全都被迫链接 painter。

现在它们在 AiPy 的 `aipy_base` 里。这是唯一「两边都要、但两边都不该拥有」
的一层：语言层按字符算长度，界面层按字符切行，谁也不是它的主人。

### 7.2 Windows 窗口外观必须与 AiUI 一致

两个库共用同一套 Windows 观感。并排看时，不该能认出哪张截图是哪个库画的。

**踩过的坑**：PyUI 早先用 `WS_POPUP` + `WM_NCCALCSIZE` 直接 `return 0`，
即「客户区 = 整窗」。这样窗口**完全没有边框与阴影**——DWM 的投影、
Win11 圆角、贴边吸附全都挂在那圈非客户区上。窗口看起来像一张直接糊在
桌面上的贴纸。

| 项 | 做法 |
|---|---|
| 窗口样式 | `WS_CAPTION + WS_THICKFRAME + WS_SYSMENU`，不用 `WS_POPUP` |
| `WM_NCCALCSIZE` | **先** `DefWindowProcW` 让系统算标准客户区，**只**把顶边抬到窗口顶端 |
| 结果 | 顶部无系统标题栏（自绘），左右与底部保留系统边框 → DWM 画边框/阴影/圆角 |
| 最大化 | 直接取 `MONITORINFO.rcWork`，比按边框厚度缩更准 |
| 缩放热区 | 左右与底部交给系统；**顶部要自己补**，那圈被吃掉画标题栏了 |

`tests/test_win_window.cpp` 的
`win_window_custom_frame_keeps_side_borders_for_shadow` 守住这条：顶部必须
与整窗对齐（否则系统标题栏会盖住自绘那条），客户区必须比整窗小（否则
没有边框与阴影）。

---

## 8. 怎么验证这些约束

| 约束 | 自动化入口 |
|---|---|
| 界面层不反向依赖语言层的实现细节 | `pyui_layers`（`tools/check_layers.py`） |
| 语言层可脱离界面运行（独立项目） | `aipy_standalone`（AiPy 里只链 `aipy` 的真实程序）+ 41 个语义用例 |
| 清单校验与依赖闭包 | `pyui_extc`（`tools/extc/check_extc.py`，8 组断言） |
| 语法覆盖、作用域、值语义 | `pyuic_syntax` / `pyuic_scope` / `pyuic_value_semantics` |
| 清单格式正确性 | `pyuic_manifest` |
| 控件绘制与像素不变量 | `pyui_tests` 里的 `test_widget_*.cpp` |
| 打包格式 | `pylib.py pack/verify`（手动，格式稳定后接入） |

「生成器错了照样能编出合法 C++」，所以前四项不能省。

---

## 9. 未决项

| 项 | 状态 |
|---|---|
| `pyui` 自身打包成 `.pylib` | 未做。分层已就绪（`pyui_core` 零 Python 依赖），缺清单与 `import pyui` 的名字表 |
| `.rsp` 真正驱动链接 | 未做。开发期靠 CMake 链全部扩展 + `/OPT:REF,ICF`；kit 场景才用精确 `.rsp` |
| macOS / Linux 后端 | 未做。界面层已无平台类型，但后端只有 Windows |
| 扩展的 AST 侧类型校验 | 部分。参数个数在 `pyuic` 校验，参数类型仍是运行期 |
