cmake_minimum_required(VERSION 3.20)
project(AiPic CXX C)

# AiPic：看图应用，可独立构建（cmake -S AiPic -B AiPic/build）。
# 依赖同级的 AiUI 库，用相对路径引入；AiUI 本身不反向依赖本工程。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "AiPic only builds with MSVC")
endif()

# 静态链接 CRT，免安装运行库
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# 同级目录的 AiUI 自绘 UI 库
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../AiUI ${CMAKE_CURRENT_BINARY_DIR}/AiUI)

add_executable(AiPic WIN32
  src/app/main.cpp
  src/app/app.cpp
  src/ui/ui_main.cpp
  src/ui/main_activity.cpp
  src/common/util.cpp
  src/res/aipic.rc
)

# .rc 里引用的 .ico 不被 CMake 跟踪：换图标后 .res 不会重编，exe 里还是旧图标。
# 显式声明依赖，让改图标一定触发重新编译。
set_source_files_properties(src/res/aipic.rc PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/res/aipic.ico")

# src 下按域分子目录，头文件按名字互相引用，不再写相对路径
target_include_directories(AiPic PRIVATE
  src
  src/app
  src/ui
  src/common
)

# XML 布局在构建期自动转成 C++ 并加入编译，改 .xml 后重新构建即可
aiui_add_layouts(AiPic
  assets/layouts/main_frame.xml
)

# 主题配色：themes.xml 里的每个 <Theme name="..."> 生成一个命名调色板
aiui_add_themes(AiPic assets/layouts/themes.xml)

# 平台入口（wWinMain / NSApplication / main）由 AiUI 提供，应用只实现 AiUI::runApp()
aiui_add_app_entry(AiPic)

target_compile_options(AiPic PRIVATE /O2 /Gy /Gw /GF /utf-8 /W4 /permissive- /Zc:__cplusplus /GL)

# 清单里声明 comctl32 v6（控件视觉风格）与 Per-Monitor V2 DPI 感知。
# 必须用 /MANIFESTINPUT 合并，直接在 .rc 里写 RT_MANIFEST 会和链接器自动生成的清单冲突。
target_link_options(AiPic PRIVATE
  /OPT:REF /OPT:ICF /LTCG /INCREMENTAL:NO
  "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/res/aipic.manifest"
)

target_link_libraries(AiPic PRIVATE
  AiUI
  comctl32
)

# 链接完就把 exe 同步到本工程的 dist\（AiPic\dist\），省得每次手动复制。
# 脚本自己判断内容，没变化时不改动文件。
add_custom_command(TARGET AiPic POST_BUILD
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/sync_dist.ps1"
          -Config "${CMAKE_BINARY_DIR}"
  COMMENT "同步 dist"
  VERBATIM
)
