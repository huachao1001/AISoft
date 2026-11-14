cmake_minimum_required(VERSION 3.20)
project(AiUI CXX)

# AiUI：自绘 UI 库
#
# 可独立构建（cmake -S AiUI -B AiUI/build -G Ninja），也可被其它工程用
# add_subdirectory 引入。公开头文件不含任何平台类型，平台细节封在
# src/os/<platform> 里。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "AiUI 目前只提供 Win32 后端，需要 MSVC 构建")
endif()

# 静态链接 CRT，发布时免装运行库
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(AIUI_SOURCES
  # 核心：视图树与唯一窗口
  src/view.cpp
  src/view_group.cpp
  src/view_root.cpp
  src/scroll_view.cpp
  src/window.cpp
  src/activity.cpp
  src/layout_inflater.cpp
  src/theme_registry.cpp
  src/font.cpp
  src/shell.cpp
  src/install.cpp

  src/events/event_dispatcher.cpp
  # 布局
  src/layouts/frame_layout.cpp
  src/layouts/linear_layout.cpp
  # 控件
  src/widgets/button.cpp
  src/widgets/button_bar.cpp
  src/widgets/breadcrumb_bar.cpp
  src/widgets/form_controls.cpp
  src/widgets/image_view.cpp
  src/widgets/input_box.cpp
  src/widgets/list_view.cpp
  src/widgets/option_view.cpp
  src/widgets/password_box.cpp
  src/widgets/status_bar.cpp
  src/widgets/text_view.cpp
  src/widgets/title_bar.cpp
  src/widgets/tree_view.cpp
)

# ── 平台后端 ──
# src/os/ 是唯一允许接触系统 API 与系统绘图库的地方。
#   src/os/os.h               系统能力的纯接口（Backend 抽象类）
#   src/os/<platform>/        每个平台实现一份，再接到 Platform 门面上
# 目前只有 Windows；macOS / Linux 各加一个分支，通用代码不用改。
if(WIN32)
  list(APPEND AIUI_SOURCES
    src/os/windows/window_windows.cpp
    src/os/windows/painter_windows.cpp
    src/os/windows/icon_windows.cpp
    src/os/windows/title_bar_windows.cpp
    src/os/windows/platform_windows.cpp
    src/os/windows/shell_menu_register.cpp
    src/os/windows/install_windows.cpp
    src/os/windows/exe_icon_windows.cpp
  )
elseif(APPLE)
  message(FATAL_ERROR "AiUI 的 macOS 后端尚未实现（src/os/macos/macos_backend.h）")
elseif(UNIX)
  message(FATAL_ERROR "AiUI 的 Linux 后端尚未实现（src/os/ubuntu/linux_backend.h）")
else()
  message(FATAL_ERROR "AiUI 没有适配当前操作系统")
endif()

add_library(AiUI STATIC ${AIUI_SOURCES})
add_library(AiUI::AiUI ALIAS AiUI)

# 头文件按 "AiUI/xxx.h" 引用，include 根就是 include/
target_include_directories(AiUI PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_definitions(AiUI PUBLIC
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  _WIN32_WINNT=0x0A00
)

target_compile_options(AiUI PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus)

target_link_libraries(AiUI PUBLIC gdiplus imm32 shell32 shlwapi ole32 oleaut32 uuid dwmapi comctl32)

# XML 布局自动生成：aiui_add_layouts(<target> assets/layouts/main.xml ...)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AiUiLayout.cmake)
# 平台入口：aiui_add_app_entry(<target>)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AiUiEntry.cmake)
# 右键菜单动态组件：aiui_add_shell_menu_dll(<target> ...)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AiUiShellMenu.cmake)

# 独立构建时顺带验证生成器可用，并编出单元测试
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()

  add_executable(AiUI_tests
    tests/test_framework.cpp
    tests/mock_platform.cpp
    tests/test_core.cpp
  )
  target_include_directories(AiUI_tests PRIVATE tests)
  target_link_libraries(AiUI_tests PRIVATE AiUI)
  target_compile_options(AiUI_tests PRIVATE /utf-8 /W4 /permissive-)
  add_test(NAME AiUI_tests COMMAND AiUI_tests)
endif()

include(GNUInstallDirs)
install(TARGETS AiUI ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES cmake/AiUiLayout.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AiUI)
