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

# 发布体积优先：静态 CRT 免除 VC 运行库依赖；链接期去掉未引用段与相同函数。
if(MSVC)
  # /utf-8：源码与执行字符集都是 UTF-8。缺了它 MSVC 会按本地代码页解释
  # 源码里的中文，字符串字面量直接变成乱码。
  add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # /OPT:REF,ICF 是体积的关键：模板与内联会让同一函数出现多份
  add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
  add_link_options($<$<CONFIG:Release>:/GL>)
  add_compile_options($<$<CONFIG:Release>:/GL>)
else()
  add_compile_options(-Wall -Wextra)
endif()

set(PYUI_SOURCES
  src/types.cpp
  src/utf8.cpp
  src/theme.cpp
  src/surface.cpp
  src/painter.cpp
  src/text_renderer.cpp
  src/view.cpp
  src/view_group.cpp
  src/view_root.cpp
  src/app_window.cpp
  src/py_value.cpp
  src/py_script.cpp
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

add_library(pyui_core STATIC ${PYUI_SOURCES})
target_include_directories(pyui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
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

# ── pyui_add_app(target SCRIPT <py> LAYOUTS <xml>... [GEN_DIR <dir>] [NO_MAIN]) ──
#
# 一条命令把一个「Python + XML」应用接起来：
#
#   1. pyuic 编译脚本 → <stem>.py.gen.cpp/.h，以及应用入口 <stem>.main.gen.cpp
#   2. pyui_layoutc 编译 XML → <stem>.gen.cpp + 汇总注册表
#   3. 生成的源文件直接进 target（静态库会被链接器丢弃，见下面的说明）
#
# NO_MAIN：不生成入口。只有「想自己写 main.cpp」的应用才需要——例如要在
# 启动前做单实例检查、自己解析命令行、或挂一个自定义托盘图标。
#
# GEN_DIR 默认是 `pyui_app_<target>`，与 pyui_add_layouts 的目录错开：
# 同一个 OUTPUT 出现在两条 add_custom_command 里，CMake 会直接判错。
function(pyui_add_app target)
  set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/pyui_app_${target})
  set(script "")
  set(xml_files)
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
            AND NOT ARGV${index} STREQUAL "NO_MAIN")
        list(APPEND xml_files ${ARGV${index}})
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

  add_custom_command(
    OUTPUT ${pyuic_outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${gen_dir}
    COMMAND ${PYUI_PYTHON} ${PYUIC_SCRIPT} ${pyuic_args} ${abs_script}
    DEPENDS ${pyuic_depends}
    COMMENT "pyuic: ${stem}.py -> ${gen_dir}"
    VERBATIM
  )

  target_sources(${target} PRIVATE ${gen_dir}/${stem}.py.gen.cpp)
  if(want_main)
    target_sources(${target} PRIVATE ${main_file})
  endif()
  target_include_directories(${target} PRIVATE ${gen_dir})
  target_link_libraries(${target} PRIVATE pyui_core)

  if(xml_files)
    pyui_add_layouts(${target} GEN_DIR ${gen_dir} ${xml_files})
  endif()
endfunction()

# ── 布局编译器 ──
add_executable(pyui_layoutc
  tools/layoutc/main.cpp
  tools/common/xml.cpp
)
target_include_directories(pyui_layoutc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/common)
target_link_libraries(pyui_layoutc PRIVATE pyui_specs)

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
  target_link_libraries(pyui_tests PRIVATE pyui_core pyui_specs)

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
  set(PYUI_EXAMPLES calculator notes progress settings)

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
      LAYOUTS ${example_dir}/assets/layouts/${example}.xml)
    set_target_properties(pyui_example_${example} PROPERTIES OUTPUT_NAME ${example})
  endforeach()
endif()
