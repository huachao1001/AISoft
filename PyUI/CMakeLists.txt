# PyUI — 自绘 UI 库 + 构建期布局编译器
#
# 目标划分（对应 AGENTS.md 的铁律）：
#   pyui_core    通用控件与绘制 + 平台后端：static lib
#   pyui_specs   属性清单：**只被工具与测试链接**，绝不进发布产物
#   pyui_layoutc XML → C++ 生成器（host 工具）
#   pyui_tests   单元测试可执行
#
# 应用只用 pyui_core + 生成代码；specs 表因此在发布产物里完全不存在。
cmake_minimum_required(VERSION 3.20)
project(pyui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ── 发布体积优先 ──
#
# /MT 静态 CRT 免除 VC 运行库依赖；/OPT:REF,ICF 在链接期去掉未引用段与
# 相同函数。/GL 让代码生成推迟到链接期，配合 /LTCG 能把跨 TU 的内联
# 做透——但它只对「在同一台机器上一起编译一起链接」成立。
#
# **分发出去的静态库必须关 /GL**（AiPy 的 aipy / aipy_base、以及本工程的
# pyui_core）：/GL 产物里是 MSIL，链接期要调 c2.dll 重新生成机器码，
# 而用户机器上只有 link.exe。给 .pylib 里的库开 /GL 等于让它在别人机器上
# 链接失败，或者报一堆看不懂的 LTCG 警告。
if(MSVC)
  # /utf-8：源码与执行字符集都是 UTF-8。缺了它 MSVC 会按本地代码页解释
  # 源码里的中文，字符串字面量直接变成乱码。
  add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
  # /GL 只给可执行文件（示例、测试、工具）：它们与库同机构建，能吃到
  # 跨 TU 优化的收益，也不会被分发出去。
  #
  # 为什么是 INTERFACE 目标而不是 add_compile_options()：目录级选项会被
  # add_subdirectory 进来的一切继承，包括同级的 AiPy。那边的库必须 /GL-，
  # 于是命令行上先是 /GL 再是 /GL-，cl 报 D9025「正在重写 /GL」。结果虽然
  # 正确，但每次全量构建刷一屏无意义警告，真警告就被埋了。
  # 显式链接到 pyui_ltcg 的目标才开 /GL，别的目录不受影响。
  # 链接期对应的是 /LTCG（/GL 是编译选项，它只是「邀请」链接器做全程序优化）。
  add_library(pyui_ltcg INTERFACE)
  target_compile_options(pyui_ltcg INTERFACE $<$<CONFIG:Release>:/GL>)
  target_link_options(pyui_ltcg INTERFACE $<$<CONFIG:Release>:/LTCG>)
else()
  add_compile_options(-Wall -Wextra)
  # 非 MSVC 平台上 /GL 不存在，留一个空目标让链接语句无条件可用。
  add_library(pyui_ltcg INTERFACE)
endif()

# 扩展静态库的平台标识，与 tools/pyuic/pylib.py 的 KNOWN_PLATFORMS 同一套名字。
# 交叉编译时「在哪构建」与「跑在哪」是两件事，所以不在这里读 uname。
if(WIN32)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(PYUI_EXT_PLATFORM "win-arm64")
  else()
    set(PYUI_EXT_PLATFORM "win-x64")
  endif()
elseif(APPLE)
  set(PYUI_EXT_PLATFORM "mac-arm64")
else()
  set(PYUI_EXT_PLATFORM "linux-x64")
endif()

set(PYUI_SOURCES
  src/types.cpp
  src/theme.cpp
  src/surface.cpp
  src/painter.cpp
  src/text_renderer.cpp
  src/view.cpp
  src/view_group.cpp
  src/view_root.cpp
  src/app_window.cpp
  src/layout_inflater.cpp
  src/software_backend.cpp
  src/layouts/linear_layout.cpp
  src/layouts/frame_layout.cpp
  src/widgets/text_view.cpp
  src/widgets/button.cpp
  src/widgets/check_box.cpp
  src/widgets/radio_button.cpp
  src/widgets/text_field.cpp
  src/widgets/progress_bar.cpp
  src/widgets/slider.cpp
  src/widgets/image_view.cpp
  src/widgets/title_bar.cpp
  src/testing/recording_painter.cpp
)

# 平台后端按操作系统选择。
#
# 这里刻意把后端源码加进 pyui_core 而不是单独一个库：平台层与通用层之间
# 是**双向**依赖（控件要 platform() 取字体，平台后端的画布要 SoftwareBackend），
# 拆成两个库只会让链接顺序变成一个必须记住的坑。
#
# 头文件仍然严格分开：include/pyui/ 里一行平台代码都没有（铁律一），
# 平台相关的头文件都在 src/os/<platform>/ 下，且用 PYUI_OS_* 宏隔离。
if(WIN32)
  list(APPEND PYUI_SOURCES
    src/os/windows/win_common.cpp
    src/os/windows/gdi_text_renderer.cpp
    src/os/windows/title_bar.cpp
    src/os/windows/win32_window.cpp
    src/os/windows/context_menu.cpp
    src/os/windows/tray.cpp
    src/os/windows/file_association.cpp
    src/os/windows/platform_windows.cpp
  )
endif()

# ── 依赖 AiPy（同级的语言运行时项目）──
#
# UTF-8 工具与 `Value` 的实现都在 AiPy 里，PyUI 只是使用者。这样做的
# 意义不是「少编一个文件」，而是让两条依赖链在物理上分开：
#
#   AiPy（同级项目，可独立构建）→ 零界面依赖
#   PyUI → AiPy（只为了用 utf8 与 Value）
#
# 如果 PyUI 自己留一份语言层副本，两边就会各自漂移：修了 AiPy 的语义
# 而 PyUI 还在跑旧的那份，症状是「同样的脚本在示例里对、在别处错」。
# 引用同一份源码就没有这个可能。
#
# 同级目录、相对路径引入，与 AiZip 依赖 AiUI 的方式一致（见 AiZip 的
# CMakeLists 头部说明）。AiPy 本身不反向依赖本工程。
set(AIPY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../AiPy)
if(NOT EXISTS ${AIPY_DIR}/CMakeLists.txt)
  message(FATAL_ERROR
    "找不到同级的 AiPy 项目：${AIPY_DIR}\n"
    "PyUI 的语言层（Value、UTF-8 工具）在 AiPy 里，两个项目必须并排放置：\n"
    "  <parent>/PyUI\n"
    "  <parent>/AiPy")
endif()
# AIPY_BUILD_TESTS 关掉：PyUI 只消费它的库，不重复跑它的测试。
set(AIPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${AIPY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/AiPy)

add_library(pyui_core STATIC ${PYUI_SOURCES})
target_include_directories(pyui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 界面层要用 UTF-8 工具（文本渲染、切片），那在 aipy_base 里。
# PUBLIC 是必须的：pyui_core 的公开头 painter.h 直接 include pyui/utf8.h，
# 而那个头 include 的是 aipy/utf8.h，任何链接 pyui_core 的目标都必须能看到
# AiPy 的 include 目录。
target_link_libraries(pyui_core PUBLIC aipy_base)
if(MSVC)
  # pyui 将来要作为 .pylib 分发（AiPy 用它做界面），同样不能带 /GL。
  target_compile_options(pyui_core PRIVATE $<$<CONFIG:Release>:/GL->)
  target_link_options(pyui_core PRIVATE $<$<CONFIG:Release>:/LTCG:OFF>)
endif()
if(WIN32)
  # 后端内部头文件（win_common.h、gdi_text_renderer.h）只对后端 TU 可见，
  # 所以是 PRIVATE 而不是 PUBLIC。
  target_include_directories(pyui_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/os/windows)
  target_compile_definitions(pyui_core PUBLIC PYUI_OS_WINDOWS)
  # dwmapi 用来探测投影/圆角（GetProcAddress 动态取，但链接器仍要看到导入表）。
  target_link_libraries(pyui_core PUBLIC
    gdi32
    user32
    shell32
    advapi32
    ole32
    imm32
    dwmapi
  )
endif()

# ── 桥接层：脚本 API ↔ 界面 ──
#
# 只有这一层同时知道 Value 和 View。它把「脚本里的 set_text(view, "x")」
# 翻译成对具体控件的调用，也把平台事件（按键、拖放、托盘）翻译回脚本。
#
# 之所以不并进 pyui_core：pyui_core 必须保持「零 Python 依赖」——那是
# 「pyui 就是一套普通 C++ UI 库」这条定位的地基。桥接层是唯一可以整体
# 替换的部分（将来打包成 .pylib 让 AiPy import 的就是它）。
add_library(pyui_script STATIC src/py_script.cpp)
target_include_directories(pyui_script PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pyui_script PUBLIC pyui_core aipy)
if(WIN32)
  target_compile_definitions(pyui_script PRIVATE PYUI_OS_WINDOWS)
endif()
if(MSVC)
  target_compile_options(pyui_script PRIVATE /W4 /utf-8)
endif()

# ── 分层：PyUI（界面） ∥ AiPy（语言运行时） ──
#
# 三个静态库，依赖方向严格单向：
#
#   aipy_base    UTF-8 工具（在 AiPy 项目里）
#   aipy         动态值 + 语言内建（在 AiPy 项目里）
#
#   pyui_core    自绘界面：控件、绘制、平台后端。**零 Python 依赖**——
#                它不知道 Value 的存在，只是一套普通 C++ UI 库。
#
#   pyui_script  桥接层：Script 基类、控件门面（setText/onClick…）、
#                菜单编码。它是唯一同时知道 Value 与 View 的地方。
#
# AiPy 是**独立的同级项目**（../AiPy），可以单独构建、单独测试、
# 单独发布。它不知道 PyUI 存在；PyUI 反过来依赖它。这是「pyui 专注
# 界面、AiPy 专注 Python 运行时」的物理落地，不是口头约定。
#
# 验证入口：
#   AiPy/tests/test_standalone.cpp  只链 aipy 的程序（在 AiPy 的 CTest 里）
#   tools/check_layers.py           本仓库侧的分层守护


# ── 体积归因探针 ──
#
# 四个目标各编一层，用 SIZE_PROBE_LEVEL 控制。它们不是产品，只在显式
# 开启时构建——默认关闭，免得每次全量构建都多编四遍。
#
# 为什么要四个独立目标而不是一个带开关的：CMake 的编译定义是 per-target
# 的，同一个源文件编出四种结果只能靠四个目标。
option(PYUI_BUILD_SIZE_PROBE "构建体积归因探针（仅用于测量预算）" OFF)
if(PYUI_BUILD_SIZE_PROBE)
  foreach(level 0 1 2 3)
    add_executable(size_probe_${level} WIN32 tools/size_probe/main.cpp)
    target_compile_definitions(size_probe_${level} PRIVATE LEVEL=${level})
    # 高层的探针要链桥接层；低层只链能编过的部分。
    target_link_libraries(size_probe_${level} PRIVATE pyui_ltcg)
    if(level GREATER 0)
      target_link_libraries(size_probe_${level} PRIVATE pyui_script)
    endif()
    set_target_properties(size_probe_${level} PROPERTIES OUTPUT_NAME size_probe_${level})
  endforeach()
endif()

# ── 属性清单：只有工具与测试链接它 ──
add_library(pyui_specs STATIC src/specs.cpp)
target_include_directories(pyui_specs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ── 脚本编译器 ──
#
# 把 Python 脚本在构建期编译成 C++。产物是 .py.gen.cpp（类实现）与
# .py.gen.h（工厂声明），所以发布产物里没有一个字节的 Python。
#
# 前端是 CPython 的 ast 模块，因此**构建期需要 Python 3.8+**。这是有意
# 的取舍：语法覆盖范围等于 CPython 本身，报错行号也来自 CPython；
# 自己手写词法/语法分析器永远追不上，且报错位置经常指错行。
# 产出的 C++ 与 Python 完全无关，运行期零依赖。
#
# PYUI_PYTHON 可以显式指定解释器：很多机器上 python 不在 PATH（本机就是
# 用 conda 环境），而构建期要求「必须能用」——找不到就明确报错，不要
# 退化成「跳过生成、链接时才炸」。
if(NOT PYUI_PYTHON)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    set(PYUI_PYTHON ${Python3_EXECUTABLE})
  endif()
endif()
if(NOT PYUI_PYTHON)
  message(FATAL_ERROR
    "找不到 Python 3.8+（脚本编译器需要它）。\n"
    "请安装 Python，或显式指定：cmake -DPYUI_PYTHON=<python.exe> ...")
endif()
message(STATUS "pyuic: 使用解释器 ${PYUI_PYTHON}")
set(PYUIC_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/tools/pyuic/pyuic.py)

# ── pyui_add_app(target SCRIPT <py> LAYOUTS <xml>... [GEN_DIR <dir>] [NO_MAIN]
#                  [EXT_DIR <dir>...]) ──
#
# 一条命令把一个「Python + XML」应用接起来：
#
#   1. pyuic 编译脚本 → <stem>.py.gen.cpp/.h，以及应用入口 <stem>.main.gen.cpp
#   2. pyuic 记录「用了哪些扩展函数」→ used.json（带行号）
#   3. pyui_extc 按清单算链接集 → app.ext.json + app.rsp
#   4. pyui_layoutc 编译 XML → <stem>.gen.cpp + 汇总注册表
#   5. 生成的源文件直接进 target（静态库会被链接器丢弃，见下面的说明）
#
# NO_MAIN：不生成入口。只有「想自己写 main.cpp」的应用才需要——例如要在
# 启动前做单实例检查、自己解析命令行、或挂一个自定义托盘图标。
#
# EXT_DIR：扩展清单所在目录（每个子目录一个 manifest.json）。不给时脚本里
# 的 import 一律报错——这是刻意的：静默忽略一个 import 会让程序启动后
# 才在调用处崩，那时已经看不出是构建期漏了一步。
#
# GEN_DIR 默认是 `pyui_app_<target>`，与 pyui_add_layouts 的目录错开：
# 同一个 OUTPUT 出现在两条 add_custom_command 里，CMake 会直接判错。
function(pyui_add_app target)
  set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/pyui_app_${target})
  set(script "")
  set(xml_files)
  set(ext_dirs)
  set(want_main TRUE)
  set(index 1)
  while(index LESS ARGC)
    if(ARGV${index} STREQUAL "SCRIPT")
      math(EXPR next "${index} + 1")
      if(next GREATER_EQUAL ARGC)
        message(FATAL_ERROR "pyui_add_app(${target}): SCRIPT 缺少路径")
      endif()
      set(script ${ARGV${next}})
      math(EXPR index "${next} + 1")
    elseif(ARGV${index} STREQUAL "LAYOUTS")
      math(EXPR index "${index} + 1")
      # LAYOUTS 后面的所有参数直到下一个关键字都是 XML
      while(index LESS ARGC
            AND NOT ARGV${index} STREQUAL "GEN_DIR"
            AND NOT ARGV${index} STREQUAL "NO_MAIN"
            AND NOT ARGV${index} STREQUAL "EXT_DIR")
        list(APPEND xml_files ${ARGV${index}})
        math(EXPR index "${index} + 1")
      endwhile()
    elseif(ARGV${index} STREQUAL "EXT_DIR")
      math(EXPR index "${index} + 1")
      while(index LESS ARGC
            AND NOT ARGV${index} STREQUAL "GEN_DIR"
            AND NOT ARGV${index} STREQUAL "NO_MAIN"
            AND NOT ARGV${index} STREQUAL "SCRIPT"
            AND NOT ARGV${index} STREQUAL "LAYOUTS")
        list(APPEND ext_dirs ${ARGV${index}})
        math(EXPR index "${index} + 1")
      endwhile()
    elseif(ARGV${index} STREQUAL "GEN_DIR")
      math(EXPR next "${index} + 1")
      if(next GREATER_EQUAL ARGC)
        message(FATAL_ERROR "pyui_add_app(${target}): GEN_DIR 缺少目录")
      endif()
      set(gen_dir ${ARGV${next}})
      math(EXPR index "${next} + 1")
    elseif(ARGV${index} STREQUAL "NO_MAIN")
      set(want_main FALSE)
      math(EXPR index "${index} + 1")
    else()
      math(EXPR index "${index} + 1")
    endif()
  endwhile()

  if(NOT script)
    message(FATAL_ERROR "pyui_add_app(${target}): 必须给出 SCRIPT")
  endif()

  get_filename_component(abs_script ${script} ABSOLUTE)
  get_filename_component(stem ${script} NAME_WE)

  # make_directory 必须在生成器之前：生成器不建目录，目录不存在时
  # 它们连一个字节都不写，构建却仍然「成功」，产物缺失要到链接才暴露。
  #
  # 同时生成三件东西：脚本类（.py.gen.cpp/.h）与应用入口（.main.gen.cpp）。
  # 入口由生成器给，源码目录里就只剩 .py 与 .xml——这正是「用户只写脚本」
  # 的落点。
  set(main_file ${gen_dir}/${stem}.main.gen.cpp)
  set(pyuic_outputs ${gen_dir}/${stem}.py.gen.cpp ${gen_dir}/${stem}.py.gen.h)
  set(pyuic_args --out ${gen_dir})
  if(want_main)
    list(APPEND pyuic_outputs ${main_file})
  else()
    list(APPEND pyuic_args --no-main)
  endif()

  # 扩展相关产物在下面算。这里先声明 used_file，是因为它由 pyuic 生成、
  # 又被 pyui_extc 消费——必须进 pyuic 的 OUTPUT 列表，否则两条命令之间
  # 没有依赖边，并行构建时 pyui_extc 可能先跑，读到不存在的 used.json。
  set(used_file ${gen_dir}/${stem}.used.json)
  set(ext_json ${gen_dir}/${stem}.ext.json)
  set(ext_rsp ${gen_dir}/${stem}.ext.rsp)
  # 入口是不是图形子系统由构建系统决定：交叉编译时「在哪构建」与
  # 「跑在哪」是两件事，不能靠 pyuic 读 sys.platform 猜。
  if(WIN32)
    list(APPEND pyuic_args --windows)
  endif()

  # 清单文件：固定叫 manifest.xml，与脚本同目录。
  #
  # 必须列进 DEPENDS，否则改清单不会触发重新生成——生成的 describe() 还是
  # 旧尺寸，症状是「明明改了窗口大小，编译也成功，窗口却没变」，排查成本很高。
  #
  # 清单可以不存在（老项目），所以用 if(EXISTS) 判断，而不是让 CMake 因为
  # 一个可有可无的输入不存在就报错。
  get_filename_component(manifest_dir ${abs_script} DIRECTORY)
  get_filename_component(manifest_file "${manifest_dir}/manifest.xml" ABSOLUTE)
  set(pyuic_depends ${PYUIC_SCRIPT} ${abs_script})
  if(EXISTS ${manifest_file})
    list(APPEND pyuic_depends ${manifest_file})
  endif()

  # ── 扩展：记录用到了什么，再按清单算链接集 ──
  #
  # 两段分开是刻意的：pyuic 有 AST 和行号，只负责「记录」；pyui_extc 是
  # 纯数据加工（C++，用户机器上也跑得动），只负责「按清单决定链接什么」。
  # schema 只有 pyui_extc 一处实现，不会漂移。
  #
  # 这一段必须在下面 add_custom_command 之前：pyuic 的 --ext-dir / --used
  # 要拼进 pyuic_args，而 pyuic_args 在命令里就被展开成字面量了。
  if(ext_dirs)
    foreach(dir ${ext_dirs})
      list(APPEND pyuic_args --ext-dir ${dir})
    endforeach()
    list(APPEND pyuic_args --used ${used_file})
    # used.json 由 pyuic 产出，必须进 OUTPUT 才有依赖边
    list(APPEND pyuic_outputs ${used_file})

    # 收集全部清单文件。用 glob 而不是要求用户逐个列出：扩展目录下有多少包
    # 会随生态变化，逐个列出迟早会漏一个——漏了的症状是「import 报没有这个
    # 模块」，而文件明明就在那，排查方向会完全错。
    set(manifest_files)
    foreach(dir ${ext_dirs})
      if(EXISTS ${dir}/manifest.json)
        list(APPEND manifest_files ${dir}/manifest.json)
      endif()
      file(GLOB subdirs RELATIVE ${dir} "${dir}/*")
      foreach(sub ${subdirs})
        if(IS_DIRECTORY ${dir}/${sub} AND EXISTS ${dir}/${sub}/manifest.json)
          list(APPEND manifest_files ${dir}/${sub}/manifest.json)
        endif()
      endforeach()
    endforeach()
    list(REMOVE_DUPLICATES manifest_files)

    # pyui_extc 的参数列表逐项拼。不用 generator expression：`--manifest`
    # 是重复出现的选项，JOIN 出来的结果在任何 CMake 版本上都不保证被拆成
    # 独立的参数项，错法的症状是「把 'a.json;--manifest;b.json' 当成一个路径」。
    set(ext_args --used ${used_file}
                 --out-json ${ext_json}
                 --out-rsp ${ext_rsp}
                 --platform ${PYUI_EXT_PLATFORM})
    foreach(manifest ${manifest_files})
      list(APPEND ext_args --manifest ${manifest})
    endforeach()
  endif()

  add_custom_command(
    OUTPUT ${pyuic_outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${gen_dir}
    COMMAND ${PYUI_PYTHON} ${PYUIC_SCRIPT} ${pyuic_args} ${abs_script}
    DEPENDS ${pyuic_depends}
    COMMENT "pyuic: ${stem}.py -> ${gen_dir}"
    VERBATIM
  )

  if(ext_dirs)
    add_custom_command(
      OUTPUT ${ext_json} ${ext_rsp}
      COMMAND pyui_extc ${ext_args}
      DEPENDS pyui_extc ${used_file} ${manifest_files}
      COMMENT "pyui_extc: 按清单算链接集"
      VERBATIM
    )
    # 孤立的自定义命令永远不会执行：它的输出必须被某个 target 需要。
    # 这里把产物作为源文件加入——CMake 会为它建立「先跑生成器」的依赖边，
    # 而 .json/.rsp 不是可编译扩展名，所以不会被编译器处理，只作为依赖存在。
    target_sources(${target} PRIVATE ${ext_json} ${ext_rsp})
    set_source_files_properties(${ext_json} ${ext_rsp} PROPERTIES
      HEADER_FILE_ONLY TRUE)
  endif()

  target_sources(${target} PRIVATE ${gen_dir}/${stem}.py.gen.cpp)
  if(want_main)
    target_sources(${target} PRIVATE ${main_file})
  endif()
  target_include_directories(${target} PRIVATE ${gen_dir})
  # 生成代码用到 py::run / py::setText 这类符号，全在桥接层。
  # 链接 pyui_script 会带进 pyui_core 与 aipy（PUBLIC 传递），
  # 所以这里不需要再逐个列。
  target_link_libraries(${target} PRIVATE pyui_script pyui_ltcg)

  if(xml_files)
    pyui_add_layouts(${target} GEN_DIR ${gen_dir} ${xml_files})
  endif()
endfunction()

# ── 扩展清单分析 ──
#
# pyui_extc 读「脚本用了哪些扩展」（pyuic 产出的 used.json）+ 各扩展的
# manifest.json，算出要链接哪些 .pylib，写出 app.ext.json 与 app.rsp。
#
# 为什么用 C++ 而不是 Python：这个工具要在**用户机器**上跑，而用户只装
# 链接器 kit，没有 Python。与 pyuic 不同——那个必须用 CPython 的 ast 才能
# 保证「语法覆盖等于 CPython」，它的前端不可替代，所以留在 Python 侧，
# 只在有 Python 的开发机上跑。
add_executable(pyui_extc
  tools/extc/main.cpp
  tools/extc/manifest.cpp
)
target_include_directories(pyui_extc PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/extc
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/third_party
)
target_link_libraries(pyui_extc PRIVATE pyui_ltcg)
# 工具链预算：pyui_extc 自身 < 1 MB。它不进发布产物，所以放宽到「几十 MB 也
# 无所谓」是不对的——kit 要整包发给用户，每多一 MB 都在安装包上。
if(MSVC)
  target_compile_options(pyui_extc PRIVATE /W4 /utf-8)
endif()

# ── 布局编译器 ──
add_executable(pyui_layoutc
  tools/layoutc/main.cpp
  tools/common/xml.cpp
)
target_include_directories(pyui_layoutc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/common)
target_link_libraries(pyui_layoutc PRIVATE pyui_specs pyui_ltcg)

# ── pyui_add_layouts(target [GEN_DIR <dir>] <XML>...) ──
#
# 把若干布局 XML 编译进 target。四个要点：
#
#   1. 每个 XML 生成独立编译单元，改一个只重编一个——这靠「生成器内容
#      相同就不写文件」保证（见 tools/layoutc/main.cpp 的 writeFile）。
#      一条命令生成全部产物，但 mtime 只会在内容真的变了的那几个上跳。
#   2. 汇总文件 pyui_layouts.gen.cpp 必须一起生成：它引用每个工厂，
#      绕开静态库「无人引用就整块丢弃」的问题。
#   3. 生成的代码作为 target 的源文件加入，不做成静态库：静态库里的工厂
#      若无人引用会被链接器整块丢弃，注册项随之消失。
#   4. GEN_DIR 可以按 target 分开。默认目录是按 target 名隔离的，但多个
#      可执行文件共用一份 BINARY_DIR 时仍建议显式给出——同一个 OUTPUT 由
#      两条 add_custom_command 声明会被 CMake 直接判为错误。
function(pyui_add_layouts target)
  # GEN_DIR 是关键字参数，其余全是 XML 路径。先用前缀解析，再按位取剩余。
  set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/pyui_generated)
  set(xml_files)
  set(index 1)
  while(index LESS ARGC)
    if(ARGV${index} STREQUAL "GEN_DIR")
      math(EXPR next "${index} + 1")
      if(next GREATER_EQUAL ARGC)
        message(FATAL_ERROR "pyui_add_layouts(${target}): GEN_DIR 缺少目录参数")
      endif()
      set(gen_dir ${ARGV${next}})
      math(EXPR index "${next} + 1")
    else()
      list(APPEND xml_files ${ARGV${index}})
      math(EXPR index "${index} + 1")
    endif()
  endwhile()

  if(NOT xml_files)
    return()
  endif()

  set(inputs)
  set(outputs)
  foreach(xml_file ${xml_files})
    get_filename_component(stem ${xml_file} NAME_WE)
    get_filename_component(abs_xml ${xml_file} ABSOLUTE)
    list(APPEND inputs ${abs_xml})
    list(APPEND outputs ${gen_dir}/${stem}.gen.cpp)
  endforeach()
  list(APPEND outputs ${gen_dir}/pyui_layouts.gen.cpp)

  # 一次调用生成全部产物，OUTPUT 列表必须与生成器实际写出的文件完全一致，
  # 否则 CMake 会认为其中某个文件没有生成规则（generate 阶段直接报错）。
  #
  # make_directory 必须排在生成器之前：生成器不会自己建目录，目录不存在时
  # 它连一个字节都不写，构建却仍然「成功」——产物缺失要到链接才暴露。
  add_custom_command(
    OUTPUT ${outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${gen_dir}
    COMMAND pyui_layoutc --out ${gen_dir} ${inputs}
    DEPENDS pyui_layoutc ${inputs}
    COMMENT "pyui_layoutc: ${target} 的 ${ARGC} 项参数 -> ${gen_dir}"
    VERBATIM
  )

  target_sources(${target} PRIVATE ${outputs})
  target_include_directories(${target} PRIVATE ${gen_dir})
  target_link_libraries(${target} PRIVATE pyui_core)
endfunction()

# ── 内置布局：编译进 pyui_core ──
#
# 标题栏的结构来自 assets/layouts/titlebar.xml，它必须是**默认可用**的：
# 应用不写任何 XML 也要有标题栏。所以它和普通布局不同，编译进 pyui_core
# 而不是某个应用。
#
# 两个要点：
#
#   1. 用 --registry-fn 生成 pyui::registerBuiltinLayouts()。静态初始化期的
#      注册项在静态库里可能被链接器整块丢弃，所以必须有一个被显式调用的
#      入口；AppWindow 构造时调一次，内置布局就一定在表里。
#   2. 应用想覆盖时，只需要在自己的 LAYOUTS 里也给一份 titlebar.xml。
#      应用注册在运行期之后（AppWindow::create 之前由 pyui_add_layouts 的
#      汇总文件写入），而 LayoutInflater::add 是后写覆盖先写——所以应用
#      那份会赢。这一点由 tests/test_layouts.cpp 断言。
set(PYUI_BUILTIN_LAYOUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pyui_builtin_layouts)
add_custom_command(
  OUTPUT ${PYUI_BUILTIN_LAYOUT_DIR}/titlebar.gen.cpp
         ${PYUI_BUILTIN_LAYOUT_DIR}/pyui_layouts.gen.cpp
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PYUI_BUILTIN_LAYOUT_DIR}
  COMMAND pyui_layoutc --out ${PYUI_BUILTIN_LAYOUT_DIR}
          --registry-fn registerBuiltinLayouts
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/layouts/titlebar.xml
  DEPENDS pyui_layoutc ${CMAKE_CURRENT_SOURCE_DIR}/assets/layouts/titlebar.xml
  COMMENT "pyui_layoutc: 内置布局 -> ${PYUI_BUILTIN_LAYOUT_DIR}"
  VERBATIM
)
target_sources(pyui_core PRIVATE
  ${PYUI_BUILTIN_LAYOUT_DIR}/titlebar.gen.cpp
  ${PYUI_BUILTIN_LAYOUT_DIR}/pyui_layouts.gen.cpp
)
target_include_directories(pyui_core PRIVATE ${PYUI_BUILTIN_LAYOUT_DIR})

# ── 应用也要拿一份内置布局 ──
#
# pyui_core 里那两份是为了让库自己「说得通」，但静态库里的目标文件只有
# 被引用时才会进产物。AppWindow 调用 registerBuiltinLayouts()，引用了那个
# 符号，所以应用会把 pyui_layouts.gen.cpp 拉进来；titlebar.gen.cpp 则被
# 显式函数引用，也会一起进链接。两条路径都成立，不需要应用再做什么。

# ── 测试 ──
option(PYUI_BUILD_TESTS "Build PyUI unit tests" ON)
if(PYUI_BUILD_TESTS)
  enable_testing()

  set(PYUI_TEST_SOURCES
    tests/test_framework.cpp
    tests/test_specs.cpp
    tests/test_widget_text_view.cpp
    tests/test_widget_button.cpp
    tests/test_widget_check_box.cpp
    tests/test_widget_radio_button.cpp
    tests/test_widget_text_field.cpp
    tests/test_widget_progress_bar.cpp
    tests/test_widget_slider.cpp
    tests/test_widget_title_bar.cpp
    tests/test_window_role.cpp
    tests/test_widget_no_native.cpp
    tests/test_layouts.cpp
    tests/test_surface.cpp
    tests/test_py_value.cpp
  )

  if(WIN32)
    # Windows 上有真实后端：titleBarAppearance() 由 src/os/windows/title_bar.cpp
    # 提供，绝不能再链接 test_platform_stubs.cpp——那是同名符号的重复定义。
    # 控件测试都显式 setAppearance()，所以真实 DPI 不影响那些期望值。
    list(APPEND PYUI_TEST_SOURCES
      tests/test_gdi_text_renderer.cpp
      tests/test_win_window.cpp
    )
  else()
    # 没有后端时用桩：TitleBar 构造会调 titleBarAppearance()，不给就链不上。
    list(APPEND PYUI_TEST_SOURCES tests/test_platform_stubs.cpp)
  endif()

  add_executable(pyui_tests ${PYUI_TEST_SOURCES})
  target_link_libraries(pyui_tests PRIVATE pyui_ltcg)
  target_include_directories(pyui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  # 内置布局（默认标题栏）必须直接编进测试可执行文件。
  #
  # 为什么不靠 pyui_core：注册发生在静态初始化期，而 pyui_core 是静态库；
  # 测试不引用 AppWindow（唯一调用 registerBuiltinLayouts 的地方），
  # 目标文件整块不会被拉进来，标题栏就 inflate 出一个 nullptr。
  # 与应用不同，测试没有「启动入口」可以挂显式调用，所以把生成单元
  # 直接编进来，让静态注册项一定存在。
  target_sources(pyui_tests PRIVATE
    ${PYUI_BUILTIN_LAYOUT_DIR}/titlebar.gen.cpp
    ${PYUI_BUILTIN_LAYOUT_DIR}/pyui_layouts.gen.cpp
  )
  target_include_directories(pyui_tests PRIVATE ${PYUI_BUILTIN_LAYOUT_DIR})
  if(WIN32)
    # test_gdi_text_renderer.cpp 要直接构造后端的渲染器来验「四处同源」，
    # 所以需要后端私有头。这只给测试，应用仍然看不到（它走 platform()）。
    target_include_directories(pyui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/os/windows)
  endif()
  # 测试里 test_py_value.cpp 直接测 Value 与 py:: 内建，那些符号在
  # aipy / pyui_script 里；单独测界面控件只需要 pyui_core。
  target_link_libraries(pyui_tests PRIVATE pyui_script pyui_specs)

  # 布局端到端测试：把 demo.xml 真的编译进来。没有这一步 test_layouts.cpp
  # 里的用例会因为 LayoutInflater::has("demo") 为假而整体跳过——链路没被
  # 验证过却显示全绿，比失败更糟。
  pyui_add_layouts(pyui_tests ${CMAKE_CURRENT_SOURCE_DIR}/assets/layouts/demo.xml)

  # 源码扫描型测试要按相对路径打开 include/，所以工作目录固定为项目根
  add_test(NAME pyui_tests COMMAND pyui_tests
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  # ── 编译器自检 ──
  #
  # 这四个脚本检查的是「生成器本身对不对」，C++ 单元测试覆盖不到：
  # 生成器错了照样能编过（错的 C++ 也是 C++）。它们必须与单元测试一起跑，
  # 否则「语法覆盖 = CPython」「作用域与 CPython 一致」这两个承诺没有守护。
  set(PYUIC_CHECK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/pyuic)
  set(PYUI_EXAMPLE_SCRIPTS
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/calculator/calculator.py
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/notes/notes.py
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/progress/progress.py
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/settings/settings.py
  )

  add_test(NAME pyuic_syntax
           COMMAND ${PYUI_PYTHON} ${PYUIC_CHECK_DIR}/check_syntax.py
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME pyuic_scope
           COMMAND ${PYUI_PYTHON} ${PYUIC_CHECK_DIR}/check_scope.py
                   ${PYUI_EXAMPLE_SCRIPTS}
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME pyuic_value_semantics
           COMMAND ${PYUI_PYTHON} ${PYUIC_CHECK_DIR}/check_value_semantics.py
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  # 清单是手写文件，所以「写错时能不能说清哪里错」本身就是要守护的能力。
  add_test(NAME pyuic_manifest
           COMMAND ${PYUI_PYTHON} ${PYUIC_CHECK_DIR}/check_manifest.py
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  # pyui_extc 决定「哪些 .pylib 进 exe」。少算一个库的症状是链接期
  # 「无法解析的外部符号」（报的是 C++ 符号名，没人会想到根因在 JSON）；
  # 多算一个是 exe 白涨体积且没有任何报错。两头都要断言。
  add_test(NAME pyui_extc
           COMMAND ${PYUI_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tools/extc/check_extc.py
                   $<TARGET_FILE:pyui_extc>
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  # 分层守护：pyui 与 AiPy 之间一旦出现反向依赖，症状是「AiPy 单独分发时
  # 突然要拖一整套界面代码」。那种退化在编译期完全看不出问题，只有真的
  # 去切一次包才会发现，所以必须自动化。
  add_test(NAME pyui_layers
           COMMAND ${PYUI_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_layers.py
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ── 示例程序 ──
#
# 每个示例只有两种文件：`<name>.py` 与 `assets/layouts/<name>.xml`。
# 入口由 pyuic 生成，示例目录里没有一行 C++——这就是「用户只写
# Python + XML」的实际样子。它们与库一起构建，所以「脚本改错了」在
# 编译期就能暴露，而不是等到某天打开示例才发现。
#
# 默认为 ON：示例就是这个库的验收用例——如果 examples 里的写法跑不通，
# 说明脚本层或宿主胶水层有问题。
option(PYUI_BUILD_EXAMPLES "Build the Python+XML example apps" ON)
if(PYUI_BUILD_EXAMPLES)
  # ── 扩展库 ──
  #
  # 扩展先于示例声明：示例要通过 EXT_LIBS 引用它们。
  #
  # 刻意**不开 /GL**：扩展库要能跨工具链分发（用户机器上只有 link.exe），
  # 而 /GL 的产物里是 MSIL，代码生成推迟到链接期，要求链接器有匹配版本的
  # c2.dll。见 spike/test_lib_ext.bat 的说明。
  add_library(pyui_ext_fs STATIC ext/fs/fs.cpp)
  target_include_directories(pyui_ext_fs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/fs/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(pyui_ext_fs PUBLIC pyui_core aipy)
  if(MSVC)
    target_compile_options(pyui_ext_fs PRIVATE $<$<CONFIG:Release>:/GL->)
  endif()
  # 输出名与位置对齐清单里写的 native.lib 路径：ext/fs/lib/win-x64/fs.lib。
  #
  # 为什么让构建产物去迁就清单，而不是让清单指向构建目录：清单是
  # **分发格式**，它描述的必须是包内的相对位置；让清单指向 build/ 会在
  # 打包时立刻失效。构建期只是借用同一个目录，方便本地跑通。
  set_target_properties(pyui_ext_fs PROPERTIES
    OUTPUT_NAME fs
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ext/fs/lib/${PYUI_EXT_PLATFORM}
    SUFFIX ".lib"
  )

  # 示例 → 它用到的扩展目标。
  #
  # 为什么要在这里重复一次「谁用哪个扩展」：CMake 在配置期就把链接行定死，
  # 而「脚本实际 import 了什么」要到构建期才由 pyuic 算出来（.rsp 是产物，
  # 不是一个能在配置期读到的输入）。所以开发期这一侧由人显式声明，而
  # kit 场景（只有链接器）才用 pyui_extc 产出的 .rsp 精确链接。
  #
  # 两边不会漂移：CMake 里多链了哪个库，/OPT:REF,ICF 会把没人引用的函数
  # 全部丢掉，体积与精确链接等价；少了哪个库，链接期立刻报未解析符号。
  # tests 里的 extc 断言守着 .rsp 的计算正确性。
  # 哪些示例用到 fs 扩展。名字与上面的目标 pyui_ext_fs 对应。
  set(PYUI_EXAMPLES_WITH_FS notes settings fileviewer)

  # 全部示例。calculator/progress 不碰文件，所以不链 fs——它们同时也是
  # 「没用扩展的程序不带扩展代码」的活的证据。
  set(PYUI_EXAMPLES calculator notes progress settings fileviewer)

  foreach(example ${PYUI_EXAMPLES})
    set(example_dir ${CMAKE_CURRENT_SOURCE_DIR}/examples/${example})
    if(NOT EXISTS ${example_dir}/${example}.py)
      message(WARNING "跳过示例 ${example}：找不到 ${example_dir}/${example}.py")
      continue()
    endif()

    # WIN32：Windows 图形子系统。缺了它链接器按控制台程序处理，双击启动
    # 会先弹一个黑框。真正的图形入口是 pyuic 生成的 wWinMain（见
    # tools/pyuic/pyuic.py 的 main_source），两者必须成对出现。
    add_executable(pyui_example_${example} WIN32)
    pyui_add_app(pyui_example_${example}
      SCRIPT ${example_dir}/${example}.py
      LAYOUTS ${example_dir}/assets/layouts/${example}.xml
      EXT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ext)
    set_target_properties(pyui_example_${example} PROPERTIES OUTPUT_NAME ${example})

    if(example IN_LIST PYUI_EXAMPLES_WITH_FS)
      target_link_libraries(pyui_example_${example} PRIVATE pyui_ext_fs)
      target_include_directories(pyui_example_${example} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ext/fs/include)
    endif()
  endforeach()
endif()
