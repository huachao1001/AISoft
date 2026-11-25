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
  src/bytecode.cpp
  # pylib 注册：靠节合并发现「已链接进来的扩展模块」。这里只有机制，
  # 没有任何具体能力（见 include/aipy/pylib.h 头部）。
  src/pylib.cpp
  # VM 按职责分文件：执行循环 / 算子 / 方法 / 命名 / 反汇编 / 热加载。
  # 执行循环刻意留在一个 switch 里（见 src/vm/machine.cpp 头部）。
  src/vm/machine.cpp
  src/vm/operators.cpp
  src/vm/methods.cpp
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

# 打包成 exe 的执行端。这是**唯一**进最终交付的二进制：用户拿到
# `aipyic.py --exe app.py` 产出的那个 exe，里面就是它 + 尾部追加的字节码。
#
# 刻意不是 GUI 子系统：控制台程序的 stdout 能直接重定向到文件，而
# `subsystem:windows` 的产物要额外 attach 到父控制台才看得见输出。
add_executable(aipy_app tools/aipy_app/main.cpp)
target_link_libraries(aipy_app PRIVATE aipy)

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
# 产物名必须是 <模块名>_pylib.lib：aipyic 的 --lib-dir 按这个名字找。
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

# ── libs/fs / libs/time / libs/json：系统库 ──
#
# Design.md 第 2.2 条：非核心能力以系统库方式提供，对齐标准 Python 接口。
# 这三个是最常用的（文件、时间、数据交换），都用与 threading 相同的两条宏
# 注册——AIPY_PYLIB_MODULE + AIPY_PYLIB_FUNCTION，没有各自的 registry。
#
# 同样每个导出函数一个编译单元：只读文件的脚本不该把 fs.open 的文件对象
# 拖进产物。
add_library(aipy_pylib_fs STATIC
  libs/fs/src/module.cpp
  libs/fs/src/io.cpp
  libs/fs/src/fn_read_text.cpp
  libs/fs/src/fn_write_text.cpp
  libs/fs/src/fn_append_text.cpp
  libs/fs/src/fn_exists.cpp
  libs/fs/src/fn_stat.cpp
  libs/fs/src/fn_size.cpp
  libs/fs/src/fn_listdir.cpp
  libs/fs/src/fn_mkdir.cpp
  libs/fs/src/fn_remove.cpp
  libs/fs/src/fn_open.cpp
)
target_include_directories(aipy_pylib_fs PUBLIC libs/fs/include)
target_link_libraries(aipy_pylib_fs PRIVATE aipy)

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

foreach(lib aipy_pylib_fs aipy_pylib_time aipy_pylib_json)
  if(MSVC)
    # 与 aipy 同一条理由：分发出的 pylib 必须关 /GL，否则用户机器上
    # 只有 link.exe、没有 c2.dll，链接会失败。
    target_compile_options(${lib} PRIVATE $<$<CONFIG:Release>:/GL->)
  endif()
endforeach()
set_target_properties(aipy_pylib_fs PROPERTIES OUTPUT_NAME "fs_pylib")
set_target_properties(aipy_pylib_time PROPERTIES OUTPUT_NAME "time_pylib")
set_target_properties(aipy_pylib_json PROPERTIES OUTPUT_NAME "json_pylib")

# ── 把 examples/ 下的每个脚本都打成 exe ──
#
# 这是交付形态的样板：构建一次就得到 4 个独立 exe，双击即可运行，
# 不需要 Python、不需要 .bc 放在旁边。
#
# 打包走 `aipyic.py --exe`（纯文件追加，见 tools/aipyic/bundle.py），
# **不编译 C++**。所以这里每加一个示例只是多一次 400 KB 的文件复制，
# 而不是多一个编译单元。
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
      COMMAND ${AIPY_PYTHON}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/aipyic.py
              --exe ${script} ${example_exe}
              --launcher $<TARGET_FILE:aipy_app>
              --build-dir ${CMAKE_BINARY_DIR}
              --lib-dir $<TARGET_FILE_DIR:aipy_pylib_threading>
      DEPENDS ${script}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/aipyic.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/bundle.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/pack.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/compiler.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/link.py
              aipy_app
              aipy_pylib_threading
              aipy_pylib_fs
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
#   编译器    tools/aipyic/*.py（脚本 → 字节码，只用标准库 ast）
#   打包      aipy_app.exe（launcher，字节码接到它尾部就是成品 exe）
#   库链接    link.py + 各个 <模块>_pylib.lib
#
# dist/ 放在**源码树**而不是构建目录：它是给人拿走的成品，构建目录随时会被
# 删掉重建。路径可用 -DAIPY_DIST_DIR=... 覆盖。
#
# 为什么用 copy_if_different 而不是 copy：dist 在源码树里，每次构建无条件
# 重写会让增量构建的产物时间戳全部变化，进而让「dist 有没有更新」失去意义。
set(AIPY_DIST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dist CACHE PATH
    "版本发布产物的输出目录")

set(AIPY_DIST_COMPILER_SOURCES
  tools/aipyic/aipyic.py
  tools/aipyic/compiler.py
  tools/aipyic/pack.py
  tools/aipyic/bundle.py
  tools/aipyic/link.py
  tools/aipyic/toolchain.py
  tools/aipyic/dist_recipe.py
)

set(AIPY_DIST_LIBS
  $<TARGET_FILE:aipy_pylib_threading>
  $<TARGET_FILE:aipy_pylib_text>
  $<TARGET_FILE:aipy_pylib_fs>
  $<TARGET_FILE:aipy_pylib_time>
  $<TARGET_FILE:aipy_pylib_json>
)

set(AIPY_DIST_COMMANDS)
foreach(source ${AIPY_DIST_COMPILER_SOURCES})
  get_filename_component(name ${source} NAME)
  list(APPEND AIPY_DIST_COMMANDS
       COMMAND ${CMAKE_COMMAND} -E copy_if_different
               ${CMAKE_CURRENT_SOURCE_DIR}/${source}
               ${AIPY_DIST_DIR}/aipyic/${name})
endforeach()

# ── 重链接配方 ──
#
# dist/ 要能独立工作：「拿到 dist 的人不装 CMake，也能把脚本打成 exe」。
# 而 link.py 平时是从 CMakeCache.txt / build.ninja 里读链接器与库清单的，
# dist 里没有这两样（它们是构建目录的产物）。
#
# 所以在构建期把 link.py 需要的事实**录下来**写进 dist/link_recipe.json，
# link.py 优先读它。这样 dist 不依赖 CMake、也不依赖源码树。
#
# 内容三项，正好是 `_read_cache` + `_find_launcher_recipe` 的输出：
#   linker    link.exe 绝对路径（dist 的持有者机器上要存在，见 README）
#   objects   launcher 的 obj —— 重新链接时作为入口
#   libraries 语言运行时与系统库 —— 与 obj 一起拼成新的 launcher
set(AIPY_DIST_LAUNCHER_OBJECT
    ${CMAKE_BINARY_DIR}/CMakeFiles/aipy_app.dir/tools/aipy_app/main.cpp.obj)

add_custom_command(
  OUTPUT ${AIPY_DIST_DIR}/link_recipe.json
  COMMAND ${AIPY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/dist_recipe.py
          --output ${AIPY_DIST_DIR}/link_recipe.json
          --linker ${CMAKE_LINKER}
          --object ${AIPY_DIST_LAUNCHER_OBJECT}
          --object-name launcher.obj
          --library ${CMAKE_BINARY_DIR}/aipy.lib
          --library ${CMAKE_BINARY_DIR}/aipy_base.lib
          --library kernel32.lib --library user32.lib --library gdi32.lib
          --library winspool.lib --library shell32.lib --library ole32.lib
          --library oleaut32.lib --library uuid.lib --library comdlg32.lib
          --library advapi32.lib
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/dist_recipe.py
          ${AIPY_DIST_LAUNCHER_OBJECT}
  COMMENT "生成 dist 的重链接配方"
  VERBATIM)

add_custom_target(aipy_dist ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${AIPY_DIST_DIR}/aipyic
  COMMAND ${CMAKE_COMMAND} -E make_directory ${AIPY_DIST_DIR}/lib
  # 清掉上次留下的 __pycache__：它是运行 aipyic 时 Python 自动写的，
  # 不是发布件。留着会把「别的 Python 版本编的 .pyc」一起发出去。
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${AIPY_DIST_DIR}/aipyic/__pycache__
  ${AIPY_DIST_COMMANDS}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:aipy_app> ${AIPY_DIST_DIR}/aipy_app.exe
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${AIPY_DIST_LAUNCHER_OBJECT} ${AIPY_DIST_DIR}/launcher.obj
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:aipy> ${AIPY_DIST_DIR}/lib/aipy.lib
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:aipy_base> ${AIPY_DIST_DIR}/lib/aipy_base.lib
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${AIPY_DIST_LIBS} ${AIPY_DIST_DIR}/lib/
  DEPENDS aipy_app aipy_pylib_threading aipy_pylib_text
          aipy_pylib_fs aipy_pylib_time aipy_pylib_json
          ${AIPY_DIST_COMPILER_SOURCES}
          ${AIPY_DIST_DIR}/link_recipe.json
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
              --output-dir ${AIPY_BYTECODE_DIR}
      COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/bytecode.stamp
      DEPENDS ${AIPY_VM_SCRIPTS}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/build_bytecode.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/compiler.py
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/pack.py
              ${CMAKE_CURRENT_SOURCE_DIR}/include/aipy/vm.h
              ${CMAKE_CURRENT_SOURCE_DIR}/src/builtins.cpp
      COMMENT "生成 VM 测试字节码"
      VERBATIM)
    add_custom_target(aipy_bytecode DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bytecode.stamp)
    add_dependencies(aipy_vm_tests aipy_bytecode)

    add_test(NAME aipy_vm_tests COMMAND aipy_vm_tests ${AIPY_BYTECODE_DIR})

    # 反汇编器与 C++ 加载器读同一份文件、各自解码。两边对不上说明格式
    # 定义或打包有 bug——这是唯一能自动发现的「静默错位」证据。
    add_test(NAME aipy_disasm
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/aipyic.py
                     --disasm
                     ${AIPY_BYTECODE_DIR}/control.bc)

    # 示例：01/02/04 与 CPython 输出逐字节比对，03 校验关键行，
    # 再把 04 打包成 exe 跑一遍（交付形态必须真的能跑）。
    # 示例过时的受害者是照着它写代码的新用户，所以它必须进测试。
    add_test(NAME aipy_examples
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_examples.py
                     $<TARGET_FILE:aipy_run>
                     $<TARGET_FILE:aipy_app>)

    # `class X(Activity)` 的展开路径：self.x → 模块变量、self.m() → m()。
    # 这条路径在 CPython 里跑不了（没有 Activity），所以不能塞进示例；
    # 但它有三处会静默算错的地方（global 检查误拦、方法前向调用、
    # 增强赋值），必须真的跑。期望值手写，用例刻意写短以便一眼核对。
    add_test(NAME aipy_activity
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_activity.py
                     $<TARGET_FILE:aipy_run>)

    # pylib 按需链接：造两个 exe（一个链了 text_pylib、一个没链），
    # 断言前者能调、后者报「没有链接进来」，且不用扩展的产物里没有
    # pylib 符号。三条都会静默出错，只能真的链接并运行来验。
    add_test(NAME aipy_pylib
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_pylib.py
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/aipyic.py
                     ${CMAKE_BINARY_DIR}
                     $<TARGET_FILE_DIR:aipy_pylib_text>
                     ${AIPY_PYTHON})

    # threading 库：功能（脚本函数真在后台跑）+ 函数级按需（只调 pending 时
    # 不该链 start/waitAll）+ 提交时校验（函数不存在立刻返回 0）。
    add_test(NAME aipy_threading
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_threading.py
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/aipyic.py
                     ${CMAKE_BINARY_DIR}
                     $<TARGET_FILE_DIR:aipy_pylib_threading>
                     ${AIPY_PYTHON})

    # 三个系统库：fs / time / json。各验功能 + 函数级按需。
    add_test(NAME aipy_system_libs
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_system_libs.py
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/aipyic.py
                     ${CMAKE_BINARY_DIR}
                     $<TARGET_FILE_DIR:aipy_pylib_fs>
                     ${AIPY_PYTHON})

    # fs.open 与 CPython open 逐行 + 逐字节对拍。
    #
    # 单列一条而不是塞进 check_system_libs：那样一份脚本要同时验「库能跑」
    # 和「语义与 CPython 一致」，失败时分不清是哪一类。期望值由 CPython
    # 现场跑出来，不是手写的。
    add_test(NAME aipy_fs_open
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_fs_open.py
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/aipyic/aipyic.py
                     ${CMAKE_BINARY_DIR}
                     $<TARGET_FILE_DIR:aipy_pylib_fs>
                     ${AIPY_PYTHON})
  endif()

  # 语义对拍：同一份脚本，CPython 跑一遍、VM 跑一遍，逐变量比较。
  #
  # 比 C++ 单测强在哪：单测的期望值是人手写的，写错就永远绿。这里的事实
  # 来源是 CPython 自己——脚本改了期望值自动跟着变。
  if(AIPY_PYTHON)
    add_test(NAME aipy_vm_semantics
             COMMAND ${AIPY_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_vm_semantics.py
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
endif()
