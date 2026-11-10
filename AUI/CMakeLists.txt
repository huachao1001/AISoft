cmake_minimum_required(VERSION 3.20)
project(AUI CXX)

# AUI：自绘 UI 库
#
# 可独立构建（cmake -S AUI -B build/AUI），也可被其它工程
# 用 add_subdirectory 引入。只依赖 Win32 GDI，不引用任何应用代码。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "AUI only builds with MSVC")
endif()

# 静态链接 CRT，免安装运行库
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(AUI STATIC
  src/button.cpp
  src/button_bar.cpp
  src/dialog.cpp
  src/font.cpp
  src/icon.cpp
  src/input_box.cpp
  src/list_view.cpp
  src/painter.cpp
  src/platform.cpp
  src/status_bar.cpp
  src/tree_view.cpp
  src/widget.cpp
)

# 头文件按 "AUI/xxx.h" 引用，include 根就是 include/
target_include_directories(AUI PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(AUI PUBLIC
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  # SetWindowSubclass / DefSubclassProc 等 comctl32 v6 API 要求
  _WIN32_WINNT=0x0601
)

target_compile_options(AUI PRIVATE /O2 /Gy /Gw /GF /utf-8 /W4 /permissive- /Zc:__cplusplus)

target_link_libraries(AUI PUBLIC
  comctl32 shell32 shlwapi ole32 oleaut32 uuid uxtheme msimg32 dwmapi
  # IME：InputBox 自绘编辑要用 Imm* API
  imm32
)

# 作为独立库对外发布：安装头文件与静态库，供其它工程 find_package(AUI) 使用
include(GNUInstallDirs)
install(TARGETS AUI ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
