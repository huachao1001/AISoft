# aiui_add_shell_menu_dll：为应用构建"文件管理器右键菜单"组件
#
# 用法：
#   aiui_add_shell_menu_dll(aizip
#     APP AiZip
#     APP_NAME "AiZip.exe"       # 组件启动的宿主可执行文件名
#     CLSID "{7B1F2C3A-5D4E-4F60-9A8B-1C2D3E4F5A6B}"
#     SOURCES src/app/shell_menu_model.cpp   # 应用里的纯逻辑实现
#   )
#
# 做了三件事：
#   1. 用平台后端的实现 + 应用的菜单模型编出组件（Windows 下是 COM DLL）；
#   2. 生成一份标识头，让宿主和组件拿到同一个 CLSID / 组件名；
#   3. 生成一个 .rc 把组件嵌进宿主 exe——分发仍然只有一个文件。
#
# 组件形态各平台不同（macOS 是 app extension，Linux 是 .desktop 描述），
# 都由本函数按平台分支，应用侧一行不用改。

function(aiui_add_shell_menu_dll target)
  cmake_parse_arguments(ARG "NO_EMBED" "APP;APP_NAME;CLSID;EMBED_INTO;CORE;MERGE_INTO" "SOURCES" ${ARGN})
  if(NOT ARG_APP OR NOT ARG_APP_NAME OR NOT ARG_CLSID OR NOT ARG_SOURCES)
    message(FATAL_ERROR
      "aiui_add_shell_menu_dll 需要 APP / APP_NAME / CLSID / SOURCES")
  endif()

  # 组件默认内嵌在应用主程序里；EMBED_INTO 可以换成别的目标（例如应用的
  # 配置 DLL），这样「注册系统集成」的代码与资源都落在同一个模块里，
  # 安装器与主程序调同一份实现。
  if(ARG_EMBED_INTO)
    set(_embed_target "${ARG_EMBED_INTO}")
  else()
    set(_embed_target "${ARG_APP}")
  endif()

  set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
  set(_out "${CMAKE_CURRENT_BINARY_DIR}")

  # MERGE_INTO：把 COM 外壳直接编进一个已有目标（例如应用的配置 dll），
  # 让「注册系统集成」的代码与 COM 类厂落在同一个模块里。安装器与主程序
  # 调同一份实现，分发时也只有一个文件。
  if(ARG_MERGE_INTO)
    set(_merge_target "${ARG_MERGE_INTO}")
    get_target_property(_merge_type ${_merge_target} TYPE)
    if(NOT _merge_type STREQUAL "SHARED_LIBRARY")
      message(FATAL_ERROR "MERGE_INTO 必须指向一个 SHARED 目标")
    endif()
  endif()

  if(WIN32)
    # ── 组件本体 ──
    if(ARG_MERGE_INTO)
      # 并进既有目标：类厂、DllMain、.def 导出一起加进去。
      # 该目标原本可能已有一份 DllMain（配置模块不需要），这里不重复加。
      target_sources(${_merge_target} PRIVATE
        "${_root}/src/os/windows/shell_menu_windows.cpp"
        "${_root}/src/os/windows/shell_menu.def"
        ${ARG_SOURCES}
      )
      target_include_directories(${_merge_target} PRIVATE
        "${_root}/include"
        "${_out}"
      )
      target_link_options(${_merge_target} PRIVATE
        "/DEF:${_root}/src/os/windows/shell_menu.def"
        /OPT:REF /OPT:ICF /INCREMENTAL:NO /MANIFEST:NO)
      target_link_libraries(${_merge_target} PRIVATE shell32 shlwapi ole32 uuid advapi32)
      set(_component_target "${_merge_target}")
    else()
      add_library(${target} SHARED
        "${_root}/src/os/windows/shell_menu_windows.cpp"
        "${_root}/src/os/windows/shell_menu.def"
        ${ARG_SOURCES}
      )
      target_include_directories(${target} PRIVATE
        "${_root}/include"
        "${_out}"
      )
      # 应用的菜单模型会引用自己的头文件（如 archive/archive.h），
      # 组件的 include 路径直接沿用宿主的，避免在调用处重复写一遍
      get_target_property(_app_includes ${ARG_APP} INCLUDE_DIRECTORIES)
      if(_app_includes)
        target_include_directories(${target} PRIVATE ${_app_includes})
      endif()
      target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
      target_compile_options(${target} PRIVATE /O2 /Gy /utf-8 /W4 /permissive- /Zc:__cplusplus)
      # .def 显式交给链接器：MSVC 才会按它导出 Dll* 入口。
      # /MANIFEST:NO：资源管理器不需要清单，少一次加载。
      target_link_options(${target} PRIVATE
        "/DEF:${_root}/src/os/windows/shell_menu.def"
        /OPT:REF /OPT:ICF /INCREMENTAL:NO /MANIFEST:NO)
      # 体积直接决定资源管理器加载它的开销
      target_link_libraries(${target} PRIVATE shell32 shlwapi ole32 uuid advapi32)
      set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${target}"
      )
      set(_component_target "${target}")
    endif()
  elseif(APPLE OR UNIX)
    message(FATAL_ERROR "AiUI 的右键菜单组件尚未支持当前平台")
  endif()

  # ── 标识头：宿主与组件必须一致 ──
  file(WRITE "${_out}/aiui_shell_menu_ids.h.in"
"// 由 aiui_add_shell_menu_dll 生成，不要手工编辑。
#pragma once

#define AIUI_SHELL_MENU_CLSID     L\"@ARG_CLSID@\"
#define AIUI_SHELL_MENU_EXE_NAME  L\"@ARG_APP_NAME@\"
#define AIUI_SHELL_MENU_COMPONENT L\"@target@\"
#define AIUI_SHELL_MENU_RESOURCE_ID 200
#define AIUI_SHELL_MENU_CORE_NAME L\"@ARG_CORE@\"
#define AIUI_SHELL_MENU_CORE_VERSION_SYMBOL \"AiuiShellMenuCoreVersion\"
#define AIUI_SHELL_MENU_CORE_BUILD_SYMBOL \"AiuiBuildShellMenuCore\"
")
  configure_file("${_out}/aiui_shell_menu_ids.h.in" "${_out}/aiui_shell_menu_ids.h" @ONLY)

  # ── 资源嵌入 ──
  # NO_EMBED：组件作为独立文件随安装包分发，不嵌进任何宿主。
  # 这条路让「外壳 dll」的名字可以固定——它不由 exe 每次释放，改名换不来
  # 任何好处，只会让安装目录堆出一串看不懂的指纹名。
  if(NOT ARG_NO_EMBED)
    # 路径必须用正斜杠：写进 C 字符串后反斜杠会被当作转义序列。
    file(TO_CMAKE_PATH "${_out}/${target}.dll" _dll_path)
    file(WRITE "${_out}/aiui_shell_menu_path.h.in"
"// 由 aiui_add_shell_menu_dll 生成，不要手工编辑。
#define AIUI_SHELL_MENU_DLL_PATH \"@_dll_path@\"
")
    configure_file("${_out}/aiui_shell_menu_path.h.in" "${_out}/aiui_shell_menu_path.h" @ONLY)

    file(WRITE "${_out}/aiui_shell_menu_embed.rc"
"// 由 aiui_add_shell_menu_dll 生成，不要手工编辑。
// RCDATA 原样写入，运行时由后端取出落盘。
#include \"aiui_shell_menu_ids.h\"
#include \"aiui_shell_menu_path.h\"

AIUI_SHELL_MENU_RESOURCE_ID RCDATA AIUI_SHELL_MENU_DLL_PATH
")
    # .rc 的编码必须是 ASCII/本地代码页，上面的内容刻意不含中文
    target_sources(${_embed_target} PRIVATE "${_out}/aiui_shell_menu_embed.rc")
    target_include_directories(${_embed_target} PRIVATE "${_out}")

    # 组件重建后必须重新打进宿主，否则升级后释放的还是旧组件
    set_source_files_properties("${_out}/aiui_shell_menu_embed.rc" PROPERTIES
      OBJECT_DEPENDS "${_out}/${target}.dll")
    add_dependencies(${_embed_target} ${_component_target})
  else()
    target_include_directories(${_component_target} PRIVATE "${_out}")
  endif()
endfunction()

# aiui_add_shell_menu_core：把应用的菜单模型编成一个可热替换的逻辑 dll
#
# 为什么单独一层：外壳被资源管理器长期持有，改不了文件；逻辑 dll 每次弹菜单
# 才加载、用完就卸，所以改文案只要换它，下次右键就是新的。
#
#   aiui_add_shell_menu_core(AiZipCore SOURCES src/app/shell_menu_model.cpp)
#
# 导出的是一套纯 C 接口（见 AiUI/shell_menu_core.h）：定长缓冲，谁分配谁释放。
function(aiui_add_shell_menu_core target)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "aiui_add_shell_menu_core 需要 SOURCES")
  endif()

  set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
  set(_out "${CMAKE_CURRENT_BINARY_DIR}")

  if(WIN32)
    add_library(${target} SHARED
      "${_root}/src/os/windows/shell_menu_core_windows.cpp"
      ${ARG_SOURCES}
    )
    target_include_directories(${target} PRIVATE
      "${_root}/include"
      "${_out}"
    )
    target_compile_definitions(${target} PRIVATE
      UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /O2 /Gy /utf-8 /W4 /permissive- /Zc:__cplusplus)
    # 逻辑层只做字符串计算：不链 shell32 / ole32，也不链 AiUI 的平台后端。
    target_link_options(${target} PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO /MANIFEST:NO)
    set_target_properties(${target} PROPERTIES PREFIX "")
  elseif(APPLE OR UNIX)
    message(FATAL_ERROR "AiUI 的右键菜单逻辑模块尚未支持当前平台")
  endif()
endfunction()
