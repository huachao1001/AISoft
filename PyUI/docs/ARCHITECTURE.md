# 架构决策：VM + 紧凑布局资源

> 状态：**已采纳**（原型验证阶段）
> 取代：`DESIGN.md` 第 3 节「XML 编译成 C++ 工厂」+ 构建期 AOT 转译

---

## 1. 决策

产品形态从「构建期把 Python 转译成 C++」改为：

| 层 | 形态 |
|---|---|
| 核心 | 基础 Python 语义 + 字节码 VM + 扩展 ABI |
| 扩展 | 独立静态库（或 DLL），带 `extension.json` 清单 |
| 布局 | 构建期校验过的紧凑二进制资源 |
| 用户源码 | **只有 `.py` 与 `.xml`** |

用户只写 Python 与 XML。其余一切由工具链负责。

---

## 2. 为什么推翻 AOT 转译

上一轮否决 VM 的依据是「300 万次循环，CPython 1010 ms、转译 3 ms，慢 340 倍」。
那个数字是真的，但**它测的是用 VM 替代 C++ 的代价，不是产品的实际需求**。

前提在讨论中变了三次，每次都在削弱 AOT 的优势：

1. **核心只是胶水**。能力全在扩展里（原生代码），脚本层负责编排调用。
   一次点击回调即使慢 20 倍，从 3 µs 变 60 µs，在 16 ms 帧预算里看不见。
2. **「速度接近 C++」与帧预算脱钩**。脚本不再承担密集计算。
3. **用户不能装 C++ 编译器**。而 AOT 必须有一个（MSVC 便携版 200 MB–2 GB）。

于是 AOT 唯一剩下的优势是「脚本层的算术更快」，而这是最不重要的一条。

---

## 3. 原型实测数据

`spike/` 下的探针可复跑：`spike\build_spike.bat`（循环/扩展）、
`spike\vm\split.cpp`（拆解分派与运算成本）。

| 项目 | 实测 |
|---|---|
| CPython 300 万次累加 | 1010 ms |
| 直接 C++ 函数调用（下界） | 1.11 ms |
| VM，`Value` = 96 字节 | 805 ms |
| VM，`Value` = 16 字节 | **477 ms** |
| 纯字节码分派（载荷是 `long long`） | 92 ms |
| 直调 `py::add`，不经解释器 | 154 ms |
| 单次扩展调用 | 94 ns |

### 修正：瓶颈不是拷贝

第一轮把 805 ms 全归给「VM 每次 push/pop 拷贝 96 字节」。瘦到 16 字节后
只到 477 ms——**假设错了**，如实记下来：

- 纯分派 92 ms，直调 `py::add` 154 ms，两者相加约 250 ms，真实 VM 477 ms。
- 说明剩下约一半开销在**指令编码与栈管理**（每条指令多次读写 `sp`、
  操作数逐个字节解码、`Value` 在栈与局部槽之间来回搬）。

`Value` 瘦身仍然值得做（96 → 16 字节，-41%），但它不是唯一原因，也不是
最后一块。要继续往 50 ms 走，得动指令编码，而不是继续揉 `Value`：

| 方向 | 预计收益 | 代价 |
|---|---|---|
| 定长指令（操作数与操作码打包成 4 字节） | 少一次解码 | 立即数范围受限 |
| 局部槽直接用指针而不是 `locals[slot]` 下标 | 少一次乘加 | 需在进入函数时定桩 |
| 常量内联（小整数不进常量池） | 少一次间接寻址 | 常量池更小，但指令更长 |

**当前结论没变**：477 ms 已比 CPython（1010 ms）快一倍，而脚本层是胶水，
这个量级在 16 ms 帧预算里看不见。50 ms 是优化目标，不是准入门槛。

---

## 4. 由此确定的三条硬约束

### 4.1 `Value` 必须 ≤ 16 字节（已落地）

实测已经做到 **16 字节**（`spike/vm/size.cpp` 打印验证）。真实实现：

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

三个关键决定，每一个都是踩过来的：

1. **堆指针必须进 union**。`std::shared_ptr` 自己就 16 字节，把它当成员会让
   整个结构体回到 32 字节——瘦身等于白做。
2. **用侵入式引用计数**（`HeapData::refs`），不用 `shared_ptr`。少一个控制块、
   少一次分配，且计数与载荷同在一块内存上。非原子：脚本只在 UI 线程跑。
3. **`bool` 不单独存字段**，存成 `kInt` 的 0/1。踩过：`Value(true)` 写
   `int_`，`toBool()` 读 `bool_`，于是 `True` 变 `False`，基准里的循环
   一次都不进，输出了一个「0.02 ms」的漂亮数字。同一件事有两个表示就一定会
   有写一个忘另一个的那天。

`std::string` 的 32 字节必须放弃——字符数据一率堆分配。代价是短字符串多一次
分配，换来每次栈操作少拷 56 字节。

### 4.2 VM 自身 `.text` < 100 KB

当前原型已满足（未优化状态下也在几十 KB 量级）。保持：
- 单字节操作码 + 定长操作数
- 一个 `switch` 分派，不用 computed goto（MSVC 不支持，且体积更大）
- 不用 `std::function`、不在热路径分配

### 4.3 扩展边界不分配

```cpp
using ExtensionFn = Value (*)(const Value* args, int count);
```

指针 + 个数，而不是 `std::vector`。DLL 与静态库能用同一个签名，
调用边界上零分配——实测 94 ns/次，其中大部分是解释器的取指与栈操作，
不是参数传递（参数只有 1 个 `Value`，16 字节）。

### 4.4 包内静态库统一用 `.pylib`

`.par` 里的静态库**不以 `.lib` 结尾**，而是一律 `.pylib`。

理由不是审美：`.par` 是跨平台分发格式，而 `.lib` / `.a` 分别是 MSVC 与 GCC
的私有约定。包内文件若跟着编译器命名，消费方就得靠扩展名猜「这是谁产的」。

前提已验证（`spike/test_lib_ext.bat`）：MSVC 的 `link.exe` 按**文件内容**
识别 COFF 归档，不看扩展名。同一个 `.pylib` 直接放进链接命令行即可：

```bat
link main.obj fs.pylib
```

`par.py` 在打包时就拒绝非 `.pylib` 的库路径，避免等到链接期才报
`LNK1104`。

---

## 5. 扩展机制

你提出的「动态新增扩展」与「链接静态库」严格来说是矛盾的，这里做个取舍：

| 模式 | 加扩展要做什么 | 适用 |
|---|---|---|
| **静态库**（默认） | 重新链接 app | 性能敏感、体积优先 |
| **DLL**（可选） | 放进目录即生效 | 用户自己写的扩展 |

两者共用同一个 ABI：

```cpp
// 静态库：链接器直接解析符号
// DLL：加载后调用导出函数注册
extern "C" __declspec(dllexport)
void pyui_extension_register(pyui::vm::Machine& machine);
```

清单文件描述「Python 名字 → C++ 符号 → 参数个数」，`pyuic` 在构建期读它，
做到：

- `import fs` 时查表，找不到就**构建期报错**（带行号 + 可用模块列表）
- 参数个数在编译期校验
- 链接哪个库由清单决定，不需要猜

扩展的分发形态是 **`.par`**（zip，类比 Android 的 AAR）：

```
fs.par
├── manifest.json                  # 模块名、版本、函数表
├── bytecode/fs.pybc               # 可选：纯 Python 部分
├── include/fs/fs.h                # 可选：C++ 头
└── lib/win-x64/fs.pylib           # 可选：静态库（统一 .pylib 后缀）
```

它是**构建期格式**：解包后静态库交给链接器、字节码嵌进 exe。产物里没有
zip 解析器，启动零成本。

**核心不含任何扩展能力**。文件读写、线程、网络全部是扩展。

> 当前过渡状态：`read_file` / `write_file` / `home_directory` / `join_path`
> 还留在 `py_script.cpp` 里（AOT 阶段的历史遗留）。VM 落地时它们要整体搬
> 出去，变成 `.par` 里的扩展——否则「核心只有基础语义」这条就名不副实。

---

## 6. 布局：紧凑资源

`pyui_layoutc` 从「生成 C++ 工厂」改为「生成二进制资源」：

| | C++ 工厂（旧） | 紧凑资源（新） |
|---|---|---|
| 每节点 | ~335 B | ~38 B |
| 属性校验 | 编译期 + layoutc 前置 | **layoutc 前置**（`pyui_specs`） |
| 热重载 | 不可能 | 可以（watch → reload） |
| 生成器 | 需要 C++ 代码生成 | 只需序列化 |

`AGENTS.md` 里选 C++ 的核心理由是「属性错误在编译期报出」——这一条已由
`pyui_layoutc` 的前置校验兜住（对着 `pyui_specs` 核对标签/属性/取值域）。
所以 C++ codegen 的独特价值只剩「多一层编译器兜底」，而代价是改布局要重编。

---

## 7. 顺带修掉的架构问题

`utf8Length` 等 UTF-8 工具原本住在 `painter.cpp` 里。这导致**任何要处理
字符串的层都被拖进整个绘制层**：脚本层算 `len("中文")`、VM、扩展库全都被迫
链接 painter。

已迁到 `src/utf8.cpp` / `include/pyui/utf8.h`，依赖方向变成：

```
value / vm / widgets → utf8 → 无
```

这样「字符串语义」的体积可以单独量，《100 KB》这个约束才有意义。

### 7.1 Windows 窗口外观必须与 AiUI 一致

两个库共用同一套 Windows 观感。并排看时，不该能认出哪张截图是哪个库画的。

**踩过的坑**：PyUI 早先用 `WS_POPUP` + `WM_NCCALCSIZE` 直接 `return 0`，
即「客户区 = 整窗」。这样窗口**完全没有边框与阴影**——因为 DWM 的投影、
Win11 圆角、贴边吸附全都挂在那圈非客户区上。窗口看起来像一张直接糊在
桌面上的贴纸。

现在的做法与 AiUI 相同：

| 项 | 做法 |
|---|---|
| 窗口样式 | `WS_CAPTION + WS_THICKFRAME + WS_SYSMENU`，不用 `WS_POPUP` |
| `WM_NCCALCSIZE` | **先** `DefWindowProcW` 让系统算标准客户区，**只**把顶边抬到窗口顶端 |
| 结果 | 顶部无系统标题栏（自绘），左右与底部保留系统边框 → DWM 画边框/阴影/圆角 |
| 最大化 | 直接取 `MONITORINFO.rcWork`，比按边框厚度缩更准（侧边任务栏 / 多显示器） |
| 缩放热区 | 左右与底部交给系统；**顶部要自己补**，那圈被吃掉画标题栏了 |
| 标题栏度量 | `bandHeight 38` / `buttonWidth 46` / `iconSize 11` / `contentInset 12` / `iconTextGap 8` |

`tests/test_win_window.cpp` 的 `win_window_custom_frame_keeps_side_borders_for_shadow`
守住这条：顶部必须与整窗对齐（否则系统标题栏会盖住自绘那条），而客户区
必须比整窗小（否则没有边框与阴影）。

---

## 8. 兼容性怎么验证

VM 路线有一个 AOT 没有的优势：**能直接跑 `.py` 文件**。

```
python script.py            > expected.txt
run.exe script.py           > actual.txt
diff expected.txt actual.txt
```

这比现在「手工写期望值再比对」强一个量级。`check_value_semantics.py`
继续保留（它验证 `py_value` 的语义），但新增端到端 diff 作为主验收手段。

---

## 9. 风险与未决项

| 风险 | 状态 |
|---|---|
| `Value` 瘦身影响现有测试 | **已消解**：249 个语义用例 + 160 平台层 + 37 Windows 后端全过 |
| VM 速度未达目标 | **部分**：477 ms（目标 50 ms）。瓶颈在指令编码，不在 `Value` |
| 现有 `pyuic` 大半要重写 | 未动。AST→C++ 的 1400 行里，作用域检查可留，代码生成要换成字节码 |
| 二进制增量待实测 | 未动。目标 < 100 KB |
| 核心里的文件 IO 未搬走 | 未动。`read_file` 等还在 `py_script.cpp`，属过渡状态 |

**下一步**：字节码编译器（`.py` → `.pybc`）。这是最后一块没动土的，而且
它一落地就能启用端到端 diff（`python x.py` vs `run.exe x.py`），把语义
验证从「手工写期望值」换成「直接比对」。
