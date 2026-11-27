cmake_minimum_required(VERSION 3.20)
project(AiPy LANGUAGES CXX)

# AiPy：Python 语义的 C++ 运行时。
#
# 定位是「一门语言的最小实现」：动态值、算术/比较/容器、字符串内建。
# 它**不认识任何界面**——没有窗口、没有控件、没有绘制。因此它可以
# 独立构建、独立链接、独立测试，也可以被任何宿主使用（GUI、命令行、
# 服务端）。PyUI 只是它众多可能的宿主之一。
#
# 这个项目能被单独拿走，证据是 `tests/test_standalone.cpp`：一个只链
# aipy 的程序，不碰任何 UI 头。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(AIPY_BUILD_TESTS "Build AiPy unit tests" ON)

# 边界守护脚本用 Python 写（与 PyUI 侧同一套检查思路）。
# 只在跑测试时需要，运行期与分发产物都不需要。
find_program(AIPY_PYTHON NAMES python3 python
             HINTS ENV PYTHON
             DOC "跑 tools/check_no_ui.py 的解释器")

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  # 静态 CRT：宿主不需要装 VC 运行库。这与「产物体积优先」是同一件事。
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(-Wall -Wextra)
endif()

# ── 核心库 ──
#
# 两个库而不是一个：utf8 是「字符串按字符处理」的工具，语义上不属于
# 语言运行时，而是它依赖的地基。分开的好处是需要 UTF-8 工具而不需要
# Value 的宿主可以只链 base。
add_library(aipy_base STATIC src/utf8.cpp)
target_include_directories(aipy_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(aipy STATIC
  src/value.cpp
  src/builtins.cpp
  src/builtins_types.cpp
  src/builtins_containers.cpp
  src/builtins_keywords.cpp
  src/builtins_identity.cpp
  src/bytecode.cpp
  # pylib 注册：靠节合并发现「已链接进来的扩展模块」。这里只有机制，
  # 没有任何具体能力（见 include/aipy/pylib.h 头部）。
  src/pylib.cpp
  # VM 按职责分文件：执行循环 / 算子 / 方法 / 命名 / 反汇编 / 热加载。
  # 执行循环刻意留在一个 switch 里（见 src/vm/machine.cpp 头部）。
  src/vm/machine.cpp
  src/vm/operators.cpp
  src/vm/methods.cpp
  src/vm/classes.cpp
  src/vm/names.cpp
  src/vm/disasm.cpp
  src/vm/state.cpp
)
target_include_directories(aipy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(aipy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(aipy PUBLIC aipy_base)

if(MSVC)
  # **分发出的静态库必须关 /GL**：/GL 产物里是 MSIL，链接期要调 c2.dll
  # 重新生成机器码。用户机器上只有 link.exe，开 /GL 等于让它在别人机器上
  # 链接失败。这一点与 PyUI 里对扩展库的要求是同一条。
  foreach(target aipy_base aipy)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/GL->)
    target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/LTCG:OFF>)
  endforeach()
endif()

# ── 独立性的活证据 ──
#
# 一个只链 aipy 的命令行程序。它证明了「这门语言不需要界面也能跑」，
# 同时也是一个可用的最小解释入口（读表达式、打印结果）。
add_executable(aipy_standalone tests/test_standalone.cpp)
target_link_libraries(aipy_standalone PRIVATE aipy)

# 跑一份字节码、把全局变量打成 JSON。语义对拍（tests/check_vm_semantics.py）
# 用它，日常调试也用它：`aipy_vm_dump app.bc` 直接看所有状态。
add_executable(aipy_vm_dump tools/aipy_vm_dump/main.cpp)
target_link_libraries(aipy_vm_dump PRIVATE aipy)

# 跑脚本、把 `print` 打到 stdout——「像 python 那样」。与 aipy_vm_dump
# 分开是因为两者的 stdout 含义不同（程序输出 vs 机器读的 JSON）。
add_executable(aipy_run tools/aipy_run/main.cpp)
target_link_libraries(aipy_run PRIVATE aipy)

# 打包成 exe 的执行端。它的**唯一**形态是静态库：`pcc --exe` 一律重链接，
# 从 `core.pylib` 里解出 `launcher.lib` 再拼字节码。
#
# 曾经还有一个 `aipy_app.exe`（纯脚本直接复制它，不用链接器）。取消它的
# 理由是**第三方库必然要链接**——既然交付目录里一定有一份能链的语言运行时，
# 就不该再维护第二个基座。代价是打包一定需要 link.exe，收益是 `dist/` 里
# 不再有两份同源的入口代码，也就没有「哪份在生效」这个问题。
#
# 刻意不是 GUI 子系统：控制台程序的 stdout 能直接重定向到文件，而
# `subsystem:windows` 的产物要额外 attach 到父控制台才看得见输出。
add_library(aipy_app_entry OBJECT tools/aipy_app/main.cpp)
target_link_libraries(aipy_app_entry PRIVATE aipy)
target_include_directories(aipy_app_entry PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(aipy_launcher STATIC $<TARGET_OBJECTS:aipy_app_entry>)
if(MSVC)
  # 与 aipy / aipy_base 同一条理由：发布出去的东西必须 /GL-，
  # 否则链接期要 c2.dll，而用户机器上只有 link.exe。
  target_compile_options(aipy_app_entry PRIVATE $<$<CONFIG:Release>:/GL->)
  target_link_options(aipy_launcher PRIVATE $<$<CONFIG:Release>:/LTCG:OFF>)
endif()

# ── 编译器：脚本 → 字节码 → exe ──
#
# 这是 `tools/aipyic/*.py` 的 C++ 移植，也是**唯一**进 dist/ 的编译器。
# 用户机器上不该有 Python：凡「打包一个脚本」需要的步骤都必须能在没有任何
# 解释器的机器上跑完，所以这一步必须是二进制。
#
# 分目录是为了「一块职责一个目录」，而不是一个几百行的 main.cpp：
#   frontend  词法 / 语法 / AST
#   codegen   AST → 字节码
#   backend   二进制格式 / 打包 / 链接 / 进程
#   shell     命令行与反汇编
#
# 它**不链 aipy**：编译器在构建期跑，不需要 VM。链上会把整个运行时
# 拖进 pcc.exe，而它只用标准库。
add_executable(pcc
  tools/pcc/src/main.cpp
  tools/pcc/src/diagnostics.cpp
  tools/pcc/src/frontend/ast.cpp
  tools/pcc/src/frontend/lexer.cpp
  tools/pcc/src/frontend/parser.cpp
  tools/pcc/src/frontend/statements.cpp
  tools/pcc/src/frontend/expressions.cpp
  tools/pcc/src/codegen/emit.cpp
  tools/pcc/src/codegen/tables.cpp
  tools/pcc/src/codegen/scope.cpp
  tools/pcc/src/codegen/module.cpp
  tools/pcc/src/codegen/statements.cpp
  tools/pcc/src/codegen/expressions.cpp
  tools/pcc/src/codegen/operators.cpp
  tools/pcc/src/backend/pack.cpp
  tools/pcc/src/backend/bundle.cpp
  tools/pcc/src/backend/fsutil.cpp
  tools/pcc/src/backend/process.cpp
  tools/pcc/src/backend/toolchain.cpp
  tools/pcc/src/backend/json.cpp
  tools/pcc/src/backend/zip.cpp
  tools/pcc/src/backend/inflate.cpp
  tools/pcc/src/backend/coff.cpp
  tools/pcc/src/backend/peimage.cpp
  tools/pcc/src/backend/pylib.cpp
  tools/pcc/src/backend/link.cpp
  tools/pcc/src/shell/disasm.cpp
  tools/pcc/src/shell/cli.cpp
)
target_include_directories(pcc PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/pcc/include
  # 异常类清单（aipy/exceptions.h）必须与运行期共用同一份：编译器认的名字
  # 和 VM 认的名字分叉，症状是「编译通过、运行期 except 永远不匹配」。
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 读 `.pylib` 需要解 deflate。用 7-Zip 的最小裁剪（third_party/7z），
# 而不是自己写 inflate：deflate 的位数与回溯拷贝是经典的正确性陷阱。
# 只有 pcc 需要它——VM 侧不解 ZIP。
add_subdirectory(third_party/7z)
target_link_libraries(pcc PRIVATE aipy_deflate)
if(MSVC)
  # 与 aipy 同一条理由：/GL 的产物要 c2.dll 才能链接，而 pcc 会被
  # 拷进 dist/ 并被别人直接运行——不过它是 exe，链接发生在构建期。
  # 这里关掉只是为了和其余分发出的产物保持一致，避免以后加库时踩坑。
  target_compile_options(pcc PRIVATE $<$<CONFIG:Release>:/GL->)
  target_link_options(pcc PRIVATE $<$<CONFIG:Release>:/LTCG:OFF>)
endif()

# ── 一个真实的 pylib，用来验证「按需链接」 ──
#
# 它不算示例（examples/ 里只许有 .py）。它是 tests/check_pylib.py 的
# 被测对象：那个测试会造两个 exe——一个链了它、一个没链——断言前者能调
# `text.upper`、后者报「没有链接进来」。所以它必须真的按 pylib 的方式
# 注册，不能用任何捷径。
#
# 同时它是**用户自定义库的样板**：用的宏与 libs/threading 完全一样
# （AIPY_PYLIB_MODULE + AIPY_PYLIB_FUNCTION），没有私有 registry。
# 三个函数各一个编译单元，`/INCLUDE` 才能精确到函数。
add_library(aipy_pylib_text STATIC
  tests/pylib/text_pylib.cpp
  tests/pylib/fn_upper.cpp
  tests/pylib/fn_repeat.cpp
  tests/pylib/fn_shout.cpp
)
target_link_libraries(aipy_pylib_text PRIVATE aipy)
if(MSVC)
  # 与 aipy 同一条理由：分发出的 pylib 必须关 /GL，否则用户机器上
  # 只有 link.exe、没有 c2.dll，链接会失败。
  target_compile_options(aipy_pylib_text PRIVATE $<$<CONFIG:Release>:/GL->)
endif()
# 产物名必须是 <模块名>_pylib.lib：pack_pylib.py 按它取原生库并打进 .pylib。
# 那个 .lib 是构建中间产物，交付形态只有 <模块>.pylib。
set_target_properties(aipy_pylib_text PROPERTIES OUTPUT_NAME "text_pylib")

# ── libs/threading：第一个「独立第三方库」样本 ──
#
# 线程不是语言语义，是具体能力，所以它在 libs/ 下、不属于 aipy.lib。
# 模块名就叫 `threading`，与 CPython 的标准库同名——脚本从 CPython 搬过来
# 时 `import threading` 不用改。
#
# **每个导出函数单独一个源文件**：MSVC 链接器按 .obj 抽取，一个 .obj 里有
# 几个函数就会一起被拉进来。函数级按需要求「用了 start 就不该把 join 拖进
# 来」，所以 fn_start.cpp / fn_thread.cpp / ... 各自独立。
#
# 注册分两层（见 libs/threading/src/module.cpp 头部）：
#   模块级  AIPY_PYLIB(threading, ...)      让 VM 认识 "threading.Thread"
#   函数级  AIPY_THREADING_FN(Thread, ...)  让打包器能 /INCLUDE 单个函数
add_library(aipy_pylib_threading STATIC
  libs/threading/src/module.cpp
  libs/threading/src/pool.cpp
  libs/threading/src/fn_start.cpp
  libs/threading/src/fn_thread.cpp
  libs/threading/src/fn_event.cpp
  libs/threading/src/fn_poll.cpp
  libs/threading/src/fn_pending.cpp
  libs/threading/src/fn_wait_all.cpp
  libs/threading/src/fn_lock.cpp
  libs/threading/src/fn_active_count.cpp
  libs/threading/src/fn_current_thread.cpp
)
target_include_directories(aipy_pylib_threading PUBLIC libs/threading/include)
target_link_libraries(aipy_pylib_threading PRIVATE aipy)
if(MSVC)
  # 与 aipy 同一条理由：分发出的 pylib 必须关 /GL，否则用户机器上
  # 只有 link.exe、没有 c2.dll，链接会失败。
  target_compile_options(aipy_pylib_threading PRIVATE $<$<CONFIG:Release>:/GL->)
endif()
set_target_properties(aipy_pylib_threading PROPERTIES OUTPUT_NAME "threading_pylib")

# ── libs/：系统库 ──
#
# Design.md 第 2.2 条：非核心能力以系统库方式提供，对齐标准 Python 接口。
# 线程、文件、目录、路径、时间、数据交换全属于这一类，都用与 threading
# 相同的宏注册——AIPY_PYLIB_MODULE + AIPY_PYLIB_FUNCTION，没有各自的 registry。
#
# 每个导出函数一个编译单元：只读文件的脚本不该把 fs.open 的文件对象拖进产物。
#
# ── 为什么先有 aipy_io ──
#
# 路径转换、存在性判断、错误描述、换行归一是 fs / os / os.path 三者共用的。
# 抽成一个库而不是复制三份：同一份代码被多个 .pylib 各带一份、一起链进产物
# 时，MSVC 的 COMDAT 折叠加会把它们并成一个（实测零警告）。所以这里不需要
# 「把共用代码塞进核心库」——核心库仍然零平台代码。
#
# 文件读写不在这里：只有 fs.open 用，放进共用层等于让每个判存在性的产物
# 都带上读写缓冲。
add_library(aipy_io OBJECT
  libs/io/src/paths.cpp
)
# PUBLIC 而不是 PRIVATE：每个用到路径的 pylib 都要能 include 它。
target_include_directories(aipy_io PUBLIC libs/io/include)
target_link_libraries(aipy_io PRIVATE aipy)
if(MSVC)
  # 这些 obj 会被编进分发出的 .pylib，所以同样必须 /GL-：用户机器上
  # 只有 link.exe，没有 c2.dll。
  target_compile_options(aipy_io PRIVATE $<$<CONFIG:Release>:/GL->)
endif()

# ── 为什么是 OBJECT 库而不是 STATIC ──
#
# 共用代码必须**留在每个 .pylib 里**，不能是一个被依赖的独立静态库。
# 静态库之间不传递：`fs.pylib` 里的 obj 引用了 `aipy_io` 的符号，而
# 链接产物时只有一个 `fs.pylib`，`aipy_io.lib` 根本不在命令行上——结果是
# LNK2001 一堆「无法解析的外部符号」（实测踩过）。
#
# OBJECT 库带的是「一组 .obj」，可以同时进多个归档。同一个 obj 被几个
# .pylib 各带一份、一起链进产物时，MSVC 的 COMDAT 折叠加会把它们并成一个
# （实测：两个库各带一份 shared.obj 链接零警告）。所以这里不需要把共用
# 代码塞进核心库——核心库仍然零平台代码。

# ── fs：`open` 与 `input` ──
#
# 两者都是 Python 的**内建**，其余原先在 fs 里的东西都是 `os` / `os.path`
# 的能力。所以这个库现在做两件事：提供文件对象、读标准输入。
#
# 它们同时用 AIPY_PYLIB_BUILTIN 注册成裸名（见 bare_open.cpp / bare_input.cpp），
# 于是脚本里写 `open(p)` / `input()` 与 `len(p)` 一样自然，而实现仍在库里：
# 核心 VM 一行文件代码、一行 stdio 代码都没有。
add_library(aipy_pylib_fs STATIC
  libs/fs/src/module.cpp
  libs/fs/src/io.cpp
  libs/fs/src/fn_open.cpp
  libs/fs/src/fn_input.cpp
  libs/fs/src/bare_open.cpp
  libs/fs/src/bare_input.cpp
)
target_include_directories(aipy_pylib_fs PUBLIC libs/fs/include)
# \$<TARGET_OBJECTS:...> 把共用 obj 直接编进这个归档：静态库之间不传递。
target_link_libraries(aipy_pylib_fs PRIVATE aipy)
target_sources(aipy_pylib_fs PRIVATE $<TARGET_OBJECTS:aipy_io>)
target_include_directories(aipy_pylib_fs PRIVATE libs/io/include)

# ── os：目录与文件系统操作 ──
#
# `listdir` / `makedirs` / `remove` 是 CPython 的 `os` 模块接口。与 `os.path`
# 分开打包：只判存在性的脚本不该把目录遍历拖进产物。
add_library(aipy_pylib_os STATIC
  libs/os/src/module.cpp
  libs/os/src/fn_listdir.cpp
  libs/os/src/fn_makedirs.cpp
  libs/os/src/fn_remove.cpp
)
target_link_libraries(aipy_pylib_os PRIVATE aipy)
target_sources(aipy_pylib_os PRIVATE $<TARGET_OBJECTS:aipy_io>)
target_include_directories(aipy_pylib_os PRIVATE libs/io/include)

# ── os.path：路径查询 ──
#
# 模块名带点，符号前缀是 `os_path`（见 libs/os_path/src/module.cpp）。
add_library(aipy_pylib_os_path STATIC
  libs/os_path/src/module.cpp
  libs/os_path/src/fn_exists.cpp
  libs/os_path/src/fn_stat.cpp
  libs/os_path/src/fn_getsize.cpp
)
target_link_libraries(aipy_pylib_os_path PRIVATE aipy)
target_sources(aipy_pylib_os_path PRIVATE $<TARGET_OBJECTS:aipy_io>)
target_include_directories(aipy_pylib_os_path PRIVATE libs/io/include)

add_library(aipy_pylib_time STATIC
  libs/time/src/module.cpp
  libs/time/src/fn_time.cpp
  libs/time/src/fn_monotonic.cpp
  libs/time/src/fn_sleep.cpp
  libs/time/src/fn_format.cpp
)
target_link_libraries(aipy_pylib_time PRIVATE aipy)

add_library(aipy_pylib_json STATIC
  libs/json/src/module.cpp
  libs/json/src/codec.cpp
  libs/json/src/fn_dumps.cpp
  libs/json/src/fn_loads.cpp
)
target_include_directories(aipy_pylib_json PUBLIC libs/json/include)
target_link_libraries(aipy_pylib_json PRIVATE aipy)

foreach(lib aipy_pylib_fs aipy_pylib_os aipy_pylib_os_path
            aipy_pylib_time aipy_pylib_json)
  if(MSVC)
    # 与 aipy 同一条理由：分发出的 pylib 必须关 /GL，否则用户机器上
    # 只有 link.exe、没有 c2.dll，链接会失败。
    target_compile_options(${lib} PRIVATE $<$<CONFIG:Release>:/GL->)
  endif()
endforeach()
set_target_properties(aipy_pylib_fs PROPERTIES OUTPUT_NAME "fs_pylib")
set_target_properties(aipy_pylib_os PROPERTIES OUTPUT_NAME "os_pylib")
set_target_properties(aipy_pylib_os_path PROPERTIES OUTPUT_NAME "os_path_pylib")
set_target_properties(aipy_pylib_time PROPERTIES OUTPUT_NAME "time_pylib")
set_target_properties(aipy_pylib_json PROPERTIES OUTPUT_NAME "json_pylib")

# ── 把 examples/ 下的每个脚本都打成 exe ──
#
# 这是交付形态的样板：构建一次就得到 4 个独立 exe，双击即可运行，
# 不需要 Python、不需要 .bc 放在旁边。
#
# 打包走 `pcc --exe`（编译 + 按需链接 + 纯文件追加），**不编译 C++**。
# 所以这里每加一个示例只是多一次几百 KB 的文件复制，而不是多一个编译单元。
if(AIPY_PYTHON)
  file(GLOB AIPY_EXAMPLE_SCRIPTS CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.py)
  set(AIPY_EXAMPLE_DIR ${CMAKE_CURRENT_BINARY_DIR}/examples)
  set(AIPY_EXAMPLE_EXES)
  foreach(script ${AIPY_EXAMPLE_SCRIPTS})
    get_filename_component(stem ${script} NAME_WE)
    set(example_exe ${AIPY_EXAMPLE_DIR}/${stem}.exe)
    add_custom_command(
      OUTPUT ${example_exe}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${AIPY_EXAMPLE_DIR}
      COMMAND $<TARGET_FILE:pcc>
              --exe ${script} ${example_exe}
              --build-dir ${CMAKE_BINARY_DIR}
              --lib-dir $<TARGET_FILE_DIR:aipy_pylib_threading>
      DEPENDS ${script}
              pcc
              aipy_launcher
              aipy_pylib_threading
              aipy_pylib_fs
              aipy_pylib_os
              aipy_pylib_os_path
              aipy_pylib_time
              aipy_pylib_json
      COMMENT "打包示例 ${stem}.exe"
      VERBATIM)
    list(APPEND AIPY_EXAMPLE_EXES ${example_exe})
  endforeach()
  # ALL：默认构建就产出，不用记得单独跑一个目标。
  add_custom_target(aipy_examples_exe ALL DEPENDS ${AIPY_EXAMPLE_EXES})
else()
  message(WARNING "找不到 Python，跳过示例打包（examples/*.exe 不会生成）")
endif()

# ── 版本发布：把交付件拷进 dist/ ──
#
# Design.md 第 3 节：发布包含「自己的 Python 编译器 + exe 打包 + .pylib 库
# 链接能力」，且每次编译后把文件拷进 dist/。
#
# 三项对应关系：
#   编译器    pcc.exe（脚本 → 字节码 / 打包 / 链接，单个二进制）
#   打包      aipy_app.exe（launcher，字节码接到它尾部就是成品 exe）
#   库链接    pcc 的 link 子命令 + 各个 <模块>.pylib
#
# **dist/ 里没有任何 .py，也没有任何 .obj**：用户机器不该装 Python，而 .obj
# 只是编译中间产物。Python 版的编译器降级成测试 oracle
# （tests/check_compiler_parity.py），只在开发机上跑。
#
# dist/ 放在**源码树**而不是构建目录：它是给人拿走的成品，构建目录随时会被
# 删掉重建。路径可用 -DAIPY_DIST_DIR=... 覆盖。
#
# 为什么用 copy_if_different 而不是 copy：dist 在源码树里，每次构建无条件
# 重写会让增量构建的产物时间戳全部变化，进而让「dist 有没有更新」失去意义。
set(AIPY_DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dist CACHE PATH
    "版本发布产物的输出目录")

# TODO(链接器打包)：dist 目前假设持有者机器上有 MSVC 的 link.exe。
# 「把链接器与 CRT 一起放进 dist」这件事单独处理，见 cmake/linkkit.cmake
# 的历史记录（实测 link.exe+依赖仅 5.2 MB，体积大头是 /MT 的 CRT 静态库）。

# ── .pylib 归档 ──
#
# 交付物是一个实体：ZIP 里带 manifest + 静态库（+ 可选的 Python 层字节码），
# 而不是「一个 .lib + 若干口头约定」。格式见 tools/pcc/include/pcc/
# backend/pylib.h。
#
# 打包含在构建期用 Python（zipfile 直接可用），读包在用户机器上用 C++
# （deflate 解码走 third_party/7z）。这与「编译器必须是 C++」是同一条约束
# 的两个方向。
#
# 函数清单从静态库自己的 COFF 符号表读，不依赖 link.exe 的输出格式——
# 当前的 link.exe 已经不认 /dump 了。
#
# ── 构建目录与 dist 用的是同一种文件 ──
#
# 打包在**构建目录**里先做一遍（`<模块>.pylib` 落在 CMAKE_BINARY_DIR），
# dist 只是把它拷过去。这样 `--lib-dir build` 与 `--lib-dir dist` 走完全
# 相同的代码路径——不留「裸 .lib 也能跑」的第二条路。
#
# 否则那条裸库路径只会在测试里活着：测试通过不代表 dist 能用。
#
# 平台目录由工具链决定（Windows `win-x64` / `.lib`，Linux `linux-x64` /
# `.a`）。`.pylib` 本身是跨平台归档，同一个包里可以有多份实现。
if(MSVC)
  set(AIPY_PYLIB_PLATFORM "win-x64")
else()
  set(AIPY_PYLIB_PLATFORM "linux-x64")
endif()

# 模块清单：三行平行列表，分别是目标名、对外模块名、是否进 dist。
#
# 两者不同是因为 `os.path` 这类点分模块名在 CMake 目标里不能有点，而
# `.pylib` 的文件名与 manifest 的 module 字段都必须是脚本里写的那个名字。
#
# 归档文件名用**模块名**（`os.path.pylib`），不是目标名——用户按脚本里的
# 模块名去找库，不该需要知道构建目标叫什么。
#
# ── 为什么 text 不发布 ──
#
# `tests/pylib/text` 是验证「按需链接真的按函数挑」的测试夹具（三个函数
# upper/repeat/shout），不是 CPython 标准库的一部分。它必须在**构建目录**
# 里产出 `.pylib`（aipy_pylib 测试要用它），但绝不能进 dist——那等于告诉
# 用户「这个库可以用」。
set(AIPY_PYLIB_TARGETS
  text threading fs os os_path time json)
set(AIPY_PYLIB_NAMES
  text threading fs os os.path time json)
set(AIPY_PYLIB_PUBLISH
  OFF ON ON ON ON ON ON)

set(AIPY_PYLIB_ARCHIVES)
set(AIPY_DIST_PYLIB_ARCHIVES)
list(LENGTH AIPY_PYLIB_TARGETS target_count)
math(EXPR last_target "${target_count} - 1")
foreach(index RANGE ${last_target})
  list(GET AIPY_PYLIB_TARGETS ${index} target_name)
  list(GET AIPY_PYLIB_NAMES ${index} module_name)
  list(GET AIPY_PYLIB_PUBLISH ${index} publish)
  set(archive ${CMAKE_BINARY_DIR}/${module_name}.pylib)
  list(APPEND AIPY_PYLIB_ARCHIVES ${archive})
  if(publish)
    list(APPEND AIPY_DIST_PYLIB_ARCHIVES ${archive})
  endif()
  add_custom_command(
    OUTPUT ${archive}
    COMMAND ${AIPY_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/pcc/pack_pylib.py
            --module ${module_name}
            --library $<TARGET_FILE:aipy_pylib_${target_name}>
            --symbol-prefix ${target_name}
            --platform ${AIPY_PYLIB_PLATFORM}
            --output ${archive}
    DEPENDS $<TARGET_FILE:aipy_pylib_${target_name}>
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/pcc/pack_pylib.py
    COMMENT "打包 ${module}.pylib"
    VERBATIM)
endforeach()

add_custom_target(aipy_pylibs ALL DEPENDS ${AIPY_PYLIB_ARCHIVES})

# ── core.pylib：语言运行时本身 ──
#
# `aipy.lib`（Value / 内建 / VM / 字节码加载器）、`aipy_base.lib`（UTF-8 与
# 平台无关工具）、`launcher.lib`（重链接入口）是**每一份产物都必须包含**的
# 东西。它们不是能力，是语言——所以不进 `libs/`、也不能像扩展库那样挑着装。
#
# 封成 `.pylib` 而不是散放在 `dist/lib` 里，是为了让发布目录的规则单一：
# **`.pylib` 都是可插拔的库，其中 `core` 是语言本身。** 散放的 `.lib` 与
# `.pylib` 长得像、语义却不同（一个是必须的、一个是可选的），混在一起没人
# 分得清。
#
# 三件套在归档里按**角色**命名（runtime / base / launcher），不是原始文件名：
# 读取端按角色取，不按文件名猜，于是「哪个是运行时、哪个是入口」是显式的。
set(AIPY_CORE_ARCHIVE ${CMAKE_BINARY_DIR}/core.pylib)
add_custom_command(
  OUTPUT ${AIPY_CORE_ARCHIVE}
  COMMAND ${AIPY_PYTHON}
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/pcc/pack_pylib.py
          --module core
          --library $<TARGET_FILE:aipy>
          --library $<TARGET_FILE:aipy_base>
          --library $<TARGET_FILE:aipy_launcher>
          --library-role runtime
          --library-role base
          --library-role launcher
          --platform ${AIPY_PYLIB_PLATFORM}
          --no-functions
          --output ${AIPY_CORE_ARCHIVE}
  DEPENDS aipy aipy_base aipy_launcher
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/pcc/pack_pylib.py
  COMMENT "打包 core.pylib（语言运行时）"
  VERBATIM)
add_custom_target(aipy_core_pylib ALL DEPENDS ${AIPY_CORE_ARCHIVE})

set(AIPY_DIST_COMMANDS)

# ── dist 不带任何配置文件 ──
#
# 曾经有一个 `link_recipe.json` 记录「链接器在哪、运行时在哪个归档、要链哪些
# 系统库」。三项逐一实测后都不该存在：
#
#   linker     是**打包那台机器**的事实，不是构建机的事实。写进发布件，
#              换台机器就失效，而对方明明装着链接器（实测过这种误报）。
#              现在由 pcc 自己找：PATH → vswhere。
#   libraries  MSVC 的 .obj 自带 /DEFAULTLIB，链接器自己会引入。实测（含
#              线程库的四份示例）去掉清单后打包与运行都正常。留着它等于
#              把 Windows 的库名固化进发布件，换平台要重打一份。
#   core       固定是 `lib/core.pylib`，每个 dist 都同一个值——写进代码。
#
# 于是 dist 是纯的「一个二进制 + 一个归档目录」，拷到任何机器都能用。
add_custom_target(aipy_dist ALL
  # dist/lib 整体重建而不是逐个删：发布目录必须精确，靠一张「要删哪些旧文件」
  # 的清单永远漏得掉一个（实测漏过 text.pylib）。全量重建只有代价是每次
  # 重拷约 1 MB，而它换来的是「目录里有什么」这件事不再依赖历史。
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${AIPY_DIST_DIR}/lib
  COMMAND ${CMAKE_COMMAND} -E make_directory ${AIPY_DIST_DIR}/lib
  # 清掉旧布局的残留：Python 版编译器目录、根上的 .pylib、以及早已被
  # lib/launcher.lib 取代的 launcher.obj；配方文件也已取消。
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${AIPY_DIST_DIR}/aipyic
  COMMAND ${CMAKE_COMMAND} -E remove
          ${AIPY_DIST_DIR}/link_recipe.json
          ${AIPY_DIST_DIR}/launcher.obj
          ${AIPY_DIST_DIR}/fs.pylib
          ${AIPY_DIST_DIR}/json.pylib
          ${AIPY_DIST_DIR}/text.pylib
          ${AIPY_DIST_DIR}/threading.pylib
          ${AIPY_DIST_DIR}/time.pylib
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:pcc> ${AIPY_DIST_DIR}/pcc.exe
  # `.pylib` 从构建目录拷过来：构建期已经打好了，dist 只是搬运。
  # 只拷要发布的那些（text 是测试夹具，不进 dist）。
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${AIPY_DIST_PYLIB_ARCHIVES} ${AIPY_DIST_DIR}/lib/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${AIPY_CORE_ARCHIVE} ${AIPY_DIST_DIR}/lib/core.pylib
  DEPENDS pcc aipy_pylibs aipy_core_pylib
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/pcc/pack_pylib.py
  COMMENT "拷贝发布件到 ${AIPY_DIST_DIR}"
  VERBATIM)

if(AIPY_BUILD_TESTS)
  enable_testing()
  add_executable(aipy_tests
    tests/test_framework.cpp
    tests/test_value.cpp
  )
  target_link_libraries(aipy_tests PRIVATE aipy)
  add_test(NAME aipy_tests COMMAND aipy_tests)
  add_test(NAME aipy_standalone COMMAND aipy_standalone)

  # VM 的端到端测试：先跑 Python 编译器生成字节码，再让 C++ 侧执行。
  # 这一步同时校验「操作码表两边一致」——顺序错一条就是全部指令错位，
  # 而症状是「结果莫名其妙」而不是崩溃。
  #
  # 字节码在**构建期**生成而不是入库：它是派生物，脚本改一行就该重新生成。
  # 曾经把它留在目录里手动生成，结果改完脚本忘了跑，测试对着旧字节码
  # 报了两次假失败。
  add_executable(aipy_vm_tests tests/test_vm.cpp)
  target_link_libraries(aipy_vm_tests PRIVATE aipy)

  # 字节码必须由 Python 编译器生成，没有解释器就跳过这几条（与
  # aipy_layers 一致）：仓库里不再存派生物，所以没有兜底路径可退。
  if(NOT AIPY_PYTHON)
    message(WARNING "找不到 Python，跳过 VM 的端到端测试")
  else()
    set(AIPY_BYTECODE_DIR ${CMAKE_CURRENT_BINARY_DIR}/bytecode)
    file(GLOB AIPY_VM_SCRIPTS CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/*.py)
    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bytecode.stamp
      COMMAND ${CMAKE_COMMAND} -E make_directory ${AIPY_BYTECODE_DIR}
      COMMAND ${AIPY_PYTHON}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/build_bytecode.py
              $<TARGET_FILE:pcc>
              --output-dir ${AIPY_BYTECODE_DIR}
      COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/bytecode.stamp
      DEPENDS ${AIPY_VM_SCRIPTS}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/build_bytecode.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/pcc/src/codegen/tables.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/include/aipy/vm.h
              ${CMAKE_CURRENT_SOURCE_DIR}/src/builtins.cpp
              pcc
      COMMENT "生成 VM 测试字节码"
      VERBATIM)
    add_custom_target(aipy_bytecode DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bytecode.stamp)
    add_dependencies(aipy_vm_tests aipy_bytecode)

    add_test(NAME aipy_vm_tests COMMAND aipy_vm_tests ${AIPY_BYTECODE_DIR})

    # 编译器移植的验收：C++ 版与 Python 版必须产出**逐字节相同**的字节码，
    # 报错文案的语义也要一致（C++ 版是英文，所以比的是翻译后的一致性）。
    #
    # Python 版在这里降级成 oracle：它不进 dist/，只当标尺。
    add_test(NAME aipy_compiler_parity
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_compiler_parity.py
                     $<TARGET_FILE:pcc>)

    # 反汇编器与 C++ 加载器读同一份文件、各自解码。两边对不上说明格式
    # 定义或打包有 bug——这是唯一能自动发现的「静默错位」证据。
    add_test(NAME aipy_disasm
             COMMAND $<TARGET_FILE:pcc> --disasm
                     ${AIPY_BYTECODE_DIR}/control.bc)

    # 示例：01/02/04 与 CPython 输出逐字节比对，03 校验关键行，
    # 再把 04 打包成 exe 跑一遍（交付形态必须真的能跑）。
    # 示例过时的受害者是照着它写代码的新用户，所以它必须进测试。
    add_test(NAME aipy_examples
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_examples.py
                     $<TARGET_FILE:pcc>
                     $<TARGET_FILE:aipy_run>
                     ${CMAKE_BINARY_DIR}
                     $<TARGET_FILE_DIR:aipy_pylib_threading>)

    # `class X(Activity)` 的展开路径：self.x → 模块变量、self.m() → m()。
    # 这条路径在 CPython 里跑不了（没有 Activity），所以不能塞进示例；
    # 但它有三处会静默算错的地方（global 检查误拦、方法前向调用、
    # 增强赋值），必须真的跑。期望值手写，用例刻意写短以便一眼核对。
    add_test(NAME aipy_activity
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_activity.py
                     $<TARGET_FILE:pcc>
                     $<TARGET_FILE:aipy_run>)

    # pylib 按需链接：造两个 exe（一个链了 text_pylib、一个没链），
    # 断言前者能调、后者报「没有链接进来」，且不用扩展的产物里没有
    # pylib 符号。三条都会静默出错，只能真的链接并运行来验。
    add_test(NAME aipy_pylib
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_pylib.py
                     $<TARGET_FILE:pcc>
                     ${CMAKE_BINARY_DIR}
                     $<TARGET_FILE_DIR:aipy_pylib_text>)

    # threading 库：功能（脚本函数真在后台跑）+ 函数级按需（只调 pending 时
    # 不该链 start/waitAll）+ 提交时校验（函数不存在立刻返回 0）。
    add_test(NAME aipy_threading
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_threading.py
                     $<TARGET_FILE:pcc>
                     ${CMAKE_BINARY_DIR}
                     $<TARGET_FILE_DIR:aipy_pylib_threading>)

    # 三个系统库：fs / time / json。各验功能 + 函数级按需。
    add_test(NAME aipy_system_libs
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_system_libs.py
                     $<TARGET_FILE:pcc>
                     ${CMAKE_BINARY_DIR}
                     $<TARGET_FILE_DIR:aipy_pylib_fs>)

    # fs.open 与 CPython open 逐行 + 逐字节对拍。
    #
    # 单列一条而不是塞进 check_system_libs：那样一份脚本要同时验「库能跑」
    # 和「语义与 CPython 一致」，失败时分不清是哪一类。期望值由 CPython
    # 现场跑出来，不是手写的。
    add_test(NAME aipy_fs_open
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_fs_open.py
                     $<TARGET_FILE:pcc>
                     ${CMAKE_BINARY_DIR}
                     $<TARGET_FILE_DIR:aipy_pylib_fs>)

    # input() 与 CPython 逐字节对拍。
    #
    # 单列一条的理由与 fs_open 相同：交互语义有三处独立的坑（提示语换行、
    # 行尾 \r、EOF 行为），混进别的测试里失败时分不清是哪一类。
    add_test(NAME aipy_input
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_input.py
                     $<TARGET_FILE:pcc>
                     ${CMAKE_BINARY_DIR}
                     $<TARGET_FILE_DIR:aipy_pylib_fs>)
  endif()

  # 语义对拍：同一份脚本，CPython 跑一遍、VM 跑一遍，逐变量比较。
  #
  # 比 C++ 单测强在哪：单测的期望值是人手写的，写错就永远绿。这里的事实
  # 来源是 CPython 自己——脚本改了期望值自动跟着变。
  if(AIPY_PYTHON)
    add_test(NAME aipy_vm_semantics
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_vm_semantics.py
                     $<TARGET_FILE:pcc>
                     $<TARGET_FILE:aipy_vm_dump>)
  endif()

  # 边界守护在**本项目自己**的 CTest 里，而不是只由消费方检查。
  # 理由见 tools/check_no_ui.py 头部：AiPy 被单独复制的场景才是最需要它的时候。
  if(AIPY_PYTHON)
    add_test(NAME aipy_layers
             COMMAND ${AIPY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_no_ui.py)
  else()
    message(WARNING "找不到 Python，跳过 aipy_layers 边界检查（不影响构建）")
  endif()

  # Design.md 第 4 节的两条：pylib 标准化（系统库与用户库同一套宏）
  # 与跨平台预留（平台细节集中在 toolchain.py）。两条退化时都不会报错，
  # 只能靠结构断言守住。
  if(AIPY_PYTHON)
    add_test(NAME aipy_standard
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_standard.py
                     ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  # dist/ 的发布契约：只有 .pylib、core 带齐三个角色、只用它就能打包。
  #
  # 为什么单列一条：任何功能测试都发现不了「发布目录里多了一个裸 .lib」——
  # 那个文件不影响构建、不影响运行，只是让人不知道该不该删。而它是真实
  # 发生过的问题（`aipy.lib` / `aipy_base.lib` / `launcher.lib` 曾被问
  # 「能不能删」，`text.pylib` 曾混进发布目录）。
  if(AIPY_PYTHON)
    add_test(NAME aipy_dist
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_dist.py
                     ${AIPY_DIST_DIR})
  endif()
endif()
