cmake_minimum_required(VERSION 3.20)
project(AiZip CXX C ASM_MASM)

# AiZip：压缩软件应用，可独立构建（cmake -S AiZip -B AiZip/build）。
# 依赖同级的 AiUI 库，用相对路径引入；AiUI 本身不反向依赖本工程。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "AiZip only builds with MSVC")
endif()

# 静态链接 CRT，免安装运行库
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# 7-Zip 内核以静态库形式编进 exe，不再需要外部 dll
add_subdirectory(third_party/7z/build-cmake)

# 同级目录的 AiUI 自绘 UI 库
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../AiUI ${CMAKE_CURRENT_BINARY_DIR}/AiUI)

# 同级目录的 AiInstaller：公共安装器核心，安装程序只做界面装配
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../AiInstaller ${CMAKE_CURRENT_BINARY_DIR}/AiInstaller)

# 7-Zip 各格式靠 *Register.cpp 里的全局对象在静态初始化阶段注册自己，这些对象
# 没有任何外部引用，普通静态库链接会被整体丢弃，结果是格式列表为空。
# /WHOLEARCHIVE 整库拉入，再由 /OPT:REF 清理真正死掉的函数。
set(Z7_LINK_OPTIONS "/WHOLEARCHIVE:7zstatic")

add_executable(AiZip WIN32
  src/app/main.cpp
  src/app/app.cpp
  src/app/config_client.cpp
  src/app/shell_menu.cpp
  src/app/single_instance.cpp
  src/archive/archive.cpp
  src/archive/sevenzip_loader.cpp
  src/archive/volumes.cpp
  src/ui/ui_main.cpp
  src/ui/main_activity.cpp
  src/ui/file_list_model.cpp
  src/ui/tree_model.cpp
  src/dialogs/dialog_activity.cpp
  src/dialogs/dlg_compress.cpp
  src/dialogs/dlg_extract.cpp
  src/dialogs/dlg_input.cpp
  src/dialogs/dlg_progress.cpp
  src/common/util.cpp
  src/res/aizip.rc
)

# .rc 里引用的 .ico 不被 CMake 跟踪：换图标后 .res 不会重编，exe 里还是旧图标。
# 显式声明依赖，让改图标一定触发重新编译。
set_source_files_properties(src/res/aizip.rc PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/res/aizip.ico")

# src 下按域分子目录，头文件按名字互相引用，不再写相对路径
target_include_directories(AiZip PRIVATE
  src
  src/app
  src/archive
  src/config
  src/ui
  src/dialogs
  src/common
  # 主程序的配置客户端与配置 dll 都要按标准 ABI 与安装器对接
  "${CMAKE_CURRENT_SOURCE_DIR}/../AiInstaller/include"
)

# XML 布局在构建期自动转成 C++ 并加入编译，改 .xml 后重新构建即可
aiui_add_layouts(AiZip
  assets/layouts/main_frame.xml
  assets/layouts/dlg_input.xml
  assets/layouts/dlg_progress.xml
  assets/layouts/dlg_extract.xml
  assets/layouts/dlg_compress.xml
)

# 主题配色：themes.xml 里的每个 <Theme name="..."> 生成一个命名调色板
aiui_add_themes(AiZip assets/layouts/themes.xml)

# 平台入口（wWinMain / NSApplication / main）由 AiUI 提供，应用只实现 AiUI::runApp()
aiui_add_app_entry(AiZip)

target_compile_options(AiZip PRIVATE /O2 /Gy /Gw /GF /utf-8 /W4 /permissive- /Zc:__cplusplus /GL)

# 清单里声明 comctl32 v6（控件视觉风格）与 Per-Monitor V2 DPI 感知。
# 必须用 /MANIFESTINPUT 合并，直接在 .rc 里写 RT_MANIFEST 会和链接器自动生成的清单冲突。
target_link_options(AiZip PRIVATE
  /OPT:REF /OPT:ICF /LTCG /INCREMENTAL:NO
  "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/res/aizip.manifest"
  ${Z7_LINK_OPTIONS}
)

target_link_libraries(AiZip PRIVATE
  AiUI
  7zstatic
  comctl32
)

# 链接完就把 exe 同步到仓库根的 dist\，省得每次手动复制。
# 脚本自己判断时间戳，没变化时不改动文件。
add_custom_command(TARGET AiZip POST_BUILD
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/sync_dist.ps1"
          -Config "${CMAKE_BINARY_DIR}"
  COMMENT "同步 dist"
  VERBATIM
)

# ── 系统配置 DLL ──
# 右键菜单与文件关联放在这里：安装器与主程序都调它，只维护一份实现。
# 独立成 dll 而不是内嵌进 exe，是为了让安装器在「尚未运行过主程序」时
# 也能完成注册，同时让主程序能随时调 Apply 自愈。
add_library(aizip_config SHARED
  src/config/aizip_config.cpp
)

target_compile_definitions(aizip_config PRIVATE
  AISETUP_CONFIG_EXPORTS
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
)
# 配置 dll 要按标准 ABI 导出，头文件在 AiInstaller 里
target_include_directories(aizip_config PRIVATE src/config
  "${CMAKE_CURRENT_SOURCE_DIR}/../AiInstaller/include")
target_compile_options(aizip_config PRIVATE /O2 /Gy /utf-8 /W4 /permissive- /Zc:__cplusplus)
target_link_options(aizip_config PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO)
target_link_libraries(aizip_config PRIVATE AiUI)

# 右键菜单的动态组件：静态动词做不出「压缩到 rnd.zip」，只能交给进程内组件。
# 组件形态、COM 注册、资源内嵌全在 AiUI 里，应用只给一份纯菜单逻辑。
#
# EMBED_INTO 指向配置 dll：注册系统集成的代码与组件资源都落在同一个模块里，
# 安装器与主程序调同一份实现，不必各自内嵌一份。
aiui_add_shell_menu_dll(aizip
  APP AiZip
  APP_NAME "AiZip.exe"
  CLSID "{7B1F2C3A-5D4E-4F60-9A8B-1C2D3E4F5A6B}"
  EMBED_INTO aizip_config
  SOURCES src/app/shell_menu_model.cpp
)

# ── 安装程序 ──
# 安装界面不再是本工程的一部分：AiInstaller 编出通用的 AiSetup.exe，
# 这里只提供一份清单（setup.ini）与要打包的文件，由 aiinstaller_pack
# 合成最终的 AiZipSetup.exe。换文案、换图标、改文件表都只动 setup.ini。
#
# 打包目录（ROOT）里放的是「按清单名字」的文件：主程序、配置 dll、界面图标。
# 这样清单不必知道各产物在构建目录里的实际位置。
set(AIZIP_SETUP_ROOT "${CMAKE_CURRENT_BINARY_DIR}/setup_root")
set(AIZIP_SETUP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/setup.ini")
set(AIZIP_SETUP_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/AiZipSetup.exe")

# 先摆好界面图标。主程序与配置 dll 由各自的构建规则在打包前复制进来，
# 所以打包依赖它们本身，而不是它们的 POST_BUILD 副作用。
add_custom_command(
  OUTPUT "${AIZIP_SETUP_ROOT}/logo.png"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${AIZIP_SETUP_ROOT}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png" "${AIZIP_SETUP_ROOT}/logo.png"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png"
  COMMENT "准备安装包图标"
  VERBATIM
)

# 打包目录里的三个文件都靠这一步就位。打成独立目标而不是 POST_BUILD：
# 打包命令依赖的是「文件已经就位」这个事实，而不是某个目标的构建副产物，
# 否则并行构建时打包可能先于复制发生，读不到文件。
add_custom_target(aizip_setup_stage ALL
  DEPENDS "${AIZIP_SETUP_ROOT}/logo.png"
)

add_dependencies(aizip_setup_stage AiZip aizip_config)

add_custom_command(TARGET aizip_setup_stage POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:AiZip>" "${AIZIP_SETUP_ROOT}/AiZip.exe"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:aizip_config>" "${AIZIP_SETUP_ROOT}/aizip_config.dll"
  COMMENT "准备安装包文件"
  VERBATIM
)

aiinstaller_add_package(
  INSTALLER AiSetup
  MANIFEST  "${AIZIP_SETUP_MANIFEST}"
  ROOT      "${AIZIP_SETUP_ROOT}"
  OUTPUT    "${AIZIP_SETUP_OUTPUT}"
  ICON      "${CMAKE_CURRENT_SOURCE_DIR}/src/res/aizip.ico"
)

# 打包目标要等主程序与配置 dll 摆进 ROOT 之后再跑
add_dependencies(AiZipSetup_package aizip_setup_stage)

# 安装包也要同步到 dist：用户实际拿到的就是它
add_custom_command(TARGET AiZipSetup_package POST_BUILD
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/sync_dist.ps1"
          -Config "${CMAKE_BINARY_DIR}"
  COMMENT "同步 dist（安装包）"
  VERBATIM
)

# 内核功能回归工具（非发布产物）：命令行驱动压缩/解压，便于端到端校验
add_executable(test_arc
  tools/test_arc.cpp
  src/archive/archive.cpp
  src/archive/sevenzip_loader.cpp
  src/archive/volumes.cpp
  src/common/util.cpp
)
target_include_directories(test_arc PRIVATE
  src
  src/archive
  src/common
)
target_link_libraries(test_arc PRIVATE AiUI)
target_compile_definitions(test_arc PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
)
target_compile_options(test_arc PRIVATE /O2 /utf-8 /W4 /permissive-)
target_link_options(test_arc PRIVATE /OPT:REF /OPT:ICF ${Z7_LINK_OPTIONS})
target_link_libraries(test_arc PRIVATE 7zstatic comctl32 shell32 shlwapi ole32 oleaut32 uuid
                                        uxtheme msimg32 dwmapi)
