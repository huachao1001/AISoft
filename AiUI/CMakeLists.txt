cmake_minimum_required(VERSION 3.20)
project(AiUI CXX)

# AiUI：自绘 UI 库
#
# 可独立构建（cmake -S AiUI -B AiUI/build -G Ninja），也可被其它工程用
# add_subdirectory 引入。公开头文件不含任何平台类型，平台细节封在
# src/win32 里。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "AiUI 目前只提供 Win32 后端，需要 MSVC 构建")
endif()

# 静态链接 CRT，发布时免装运行库
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(AIUI_SOURCES
  # 核心
  src/widget.cpp
  src/font.cpp
  # 布局
  src/layouts/frame_layout.cpp
  src/layouts/linear_layout.cpp
  # 控件
  src/widgets/button.cpp
  src/widgets/button_bar.cpp
  src/widgets/breadcrumb_bar.cpp
  src/widgets/dialog.cpp
  src/widgets/image_view.cpp
  src/widgets/input_box.cpp
  src/widgets/list_view.cpp
  src/widgets/status_bar.cpp
  src/widgets/text_view.cpp
  src/widgets/tree_view.cpp
  # Win32 后端：唯一接触 Windows API 与 GDI+ 的地方
  src/win32/window_win32.cpp
  src/win32/painter_win32.cpp
  src/win32/icon_win32.cpp
)

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

target_link_libraries(AiUI PUBLIC gdiplus imm32 shell32 shlwapi ole32 oleaut32 uuid)

# XML 布局自动生成：aiui_add_layouts(<target> assets/layouts/main.xml ...)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AiUiLayout.cmake)

# 独立构建时顺带验证生成器可用
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
endif()

include(GNUInstallDirs)
install(TARGETS AiUI ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES cmake/AiUiLayout.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AiUI)
