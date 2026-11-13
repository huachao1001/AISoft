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

# 7-Zip 各格式靠 *Register.cpp 里的全局对象在静态初始化阶段注册自己，这些对象
# 没有任何外部引用，普通静态库链接会被整体丢弃，结果是格式列表为空。
# /WHOLEARCHIVE 整库拉入，再由 /OPT:REF 清理真正死掉的函数。
set(Z7_LINK_OPTIONS "/WHOLEARCHIVE:7zstatic")

add_executable(AiZip WIN32
  src/app/main.cpp
  src/app/app.cpp
  src/app/shell_menu.cpp
  src/app/shell_menu_model.cpp
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
  src/ui
  src/dialogs
  src/common
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

# 右键菜单的动态组件：静态动词做不出「压缩到 rnd.zip」，只能交给进程内组件。
# 组件形态、COM 注册、资源内嵌全在 AiUI 里，应用只给一份纯菜单逻辑。
aiui_add_shell_menu_dll(aicmd
  APP AiZip
  APP_NAME "AiZip.exe"
  CLSID "{7B1F2C3A-5D4E-4F60-9A8B-1C2D3E4F5A6B}"
  SOURCES src/app/shell_menu_model.cpp
)

# ── 安装程序 ──
# 自建安装包：只做「把文件放到目标目录 + 写注册表」。注册表那部分不在这里
# 重写，而是调用已落地的 exe 自己注册（组件是它的内嵌资源）。
#
# 内嵌的程序要在构建期才存在，所以 payload.rc 由脚本生成，主 .rc 再包含它。
set(SETUP_PAYLOAD_RC "${CMAKE_CURRENT_BINARY_DIR}/res/setup_payload.rc")
add_custom_command(
  OUTPUT "${SETUP_PAYLOAD_RC}"
  COMMAND ${CMAKE_COMMAND}
          -DEXE=$<TARGET_FILE:AiZip>
          -DOUT=${SETUP_PAYLOAD_RC}
          -DRC=${CMAKE_CURRENT_SOURCE_DIR}/src/res/setup.rc
          -P "${CMAKE_CURRENT_SOURCE_DIR}/../AiUI/cmake/aiui_setup_rc.cmake"
  DEPENDS AiZip "${CMAKE_CURRENT_SOURCE_DIR}/../AiUI/cmake/aiui_setup_rc.cmake"
  COMMENT "生成安装包内嵌资源"
  VERBATIM
)

add_executable(AiZipSetup WIN32
  src/setup/main.cpp
  src/setup/install_activity.cpp
  src/res/setup.rc
)

# .rc 里 #include 的生成文件不被 CMake 跟踪，显式声明依赖，
# 让 ninja 先跑生成规则再编译资源
set_source_files_properties(src/res/setup.rc PROPERTIES
  OBJECT_DEPENDS "${SETUP_PAYLOAD_RC}")

target_include_directories(AiZipSetup PRIVATE
  src/setup
  src
  "${CMAKE_CURRENT_BINARY_DIR}/res"
)

aiui_add_layouts(AiZipSetup assets/layouts/dlg_install.xml
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/aiui_generated_setup")
aiui_add_themes(AiZipSetup assets/layouts/themes.xml
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/aiui_generated_setup")
aiui_add_app_entry(AiZipSetup)

target_compile_options(AiZipSetup PRIVATE /O2 /utf-8 /W4 /permissive-)
target_link_options(AiZipSetup PRIVATE
  /OPT:REF /OPT:ICF
  "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/res/aizip.manifest"
)
target_link_libraries(AiZipSetup PRIVATE AiUI comctl32)

# 安装包也要同步到 dist：用户实际拿到的就是它
add_custom_command(TARGET AiZipSetup POST_BUILD
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
