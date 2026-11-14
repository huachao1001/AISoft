cmake_minimum_required(VERSION 3.20)
project(AiInstaller CXX)

# AiInstaller：通用安装器
#
# 和 AiUI 一样的定位：谁也不反向依赖。它产出两样东西：
#
#   AiSetup.exe       通用安装器（固定界面，读自己末尾的包）
#   aiinstaller_pack  打包器（把清单与文件追加到安装器副本上）
#
# 应用只需要写一份清单（setup.ini），然后调 aiinstaller_add_package 打包，
# 不必为安装界面写一行代码，也不必重新编译安装器。
#
# 它负责所有应用共通的安装事实：
#   - 装到哪个盘、哪个目录（跳过系统盘的策略内嵌在这里）
#   - 落盘、快捷方式、卸载入口、卸载收尾
#   - 提权与提权后的参数还原
#
# 应用私有的系统配置（右键菜单、文件关联）不在这里，通过 config_abi.h 的
# 标准 ABI 交给安装目录里的配置 dll。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "AiInstaller 目前只实现了 Windows 后端")
endif()

# 静态链接 CRT，免安装运行库
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# AiUI 提供全部系统能力的中性接口：落盘、快捷方式、卸载入口、提权、图片
if(NOT TARGET AiUI)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../AiUI ${CMAKE_CURRENT_BINARY_DIR}/AiUI)
endif()

# ── 通用安装器可执行文件 ──
#
# 它不链接任何应用代码：装什么完全由运行期读到的清单决定。
add_executable(AiSetup WIN32
  src/ui/setup_main.cpp
  src/ui/setup_activity.cpp
  src/install.cpp
  src/manifest.cpp
  src/package.cpp
  src/path_policy.cpp
  src/config_module.cpp
  src/internal_util.cpp
  src/os/windows/installer_windows.cpp
  src/res/setup.rc
)

set_source_files_properties(src/res/setup.rc PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/res/aicon.ico")

target_include_directories(AiSetup PRIVATE
  src
  src/ui
  include
)

# 界面布局在构建期编译成 C++（AiUI 不做运行时 XML 解析）。
# 整个安装器只有这一份模板，应用改的是清单里的文案。
aiui_add_layouts(AiSetup assets/layouts/dlg_setup.xml
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/aiui_generated")
aiui_add_themes(AiSetup assets/layouts/themes.xml
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/aiui_generated")
aiui_add_app_entry(AiSetup)

target_compile_options(AiSetup PRIVATE /O2 /Gy /utf-8 /W4 /permissive- /Zc:__cplusplus)
target_compile_definitions(AiSetup PRIVATE UNICODE _UNICODE)
target_link_options(AiSetup PRIVATE
  /OPT:REF /OPT:ICF
  "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/res/setup.manifest"
)
target_link_libraries(AiSetup PRIVATE AiUI comctl32)

# ── 打包器 ──
#
# 只做文件与资源操作，不链接 AiUI：它在构建期跑，没必要拖进整套图形库。
add_executable(aiinstaller_pack
  tools/pack/pack_main.cpp
  src/manifest.cpp
)

target_include_directories(aiinstaller_pack PRIVATE include)
# 必须定义 UNICODE：RT_ICON / RT_GROUP_ICON 这类资源类型宏要展开成宽字符版本，
# 否则 UpdateResourceW 收到的类型字符串类型对不上，直接编译失败。
target_compile_definitions(aiinstaller_pack PRIVATE UNICODE _UNICODE)
target_compile_options(aiinstaller_pack PRIVATE /O2 /utf-8 /W4 /permissive- /Zc:__cplusplus)
target_link_libraries(aiinstaller_pack PRIVATE shell32)

# 输出的 exe 名就叫 AiSetup.exe：它被复制成各种名字（AiZipSetup.exe 等），
# 但原件只有一份。
set_target_properties(AiSetup PROPERTIES OUTPUT_NAME "AiSetup")
set_target_properties(aiinstaller_pack PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AiInstallerPackage.cmake)
