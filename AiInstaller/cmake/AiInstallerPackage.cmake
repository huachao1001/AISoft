# aiinstaller_add_package：把清单与文件打包成一个成品安装包
#
# 用法（应用侧）：
#   aiinstaller_add_package(
#     INSTALLER   AiSetup                  # 通用安装器目标
#     MANIFEST    setup.ini                # 应用写的清单
#     ROOT        "$<TARGET_FILE_DIR:AiZip>"  # 清单里的文件相对哪里找
#     OUTPUT      AiZipSetup.exe           # 产物
#     DEPENDS     AiZip aizip_config       # 产物要等它们先构建完
#     ICON        src/res/aizip.ico        # 可选：安装包自己的图标
#   )
#
# 为什么放到构建期执行：待打包的 exe/dll 要到构建期才存在，配置期拿不到。
function(aiinstaller_add_package)
  cmake_parse_arguments(ARG "" "INSTALLER;MANIFEST;ROOT;OUTPUT;ICON;WORK_DIR" "DEPENDS" ${ARGN})

  if(NOT ARG_INSTALLER OR NOT ARG_MANIFEST OR NOT ARG_ROOT OR NOT ARG_OUTPUT)
    message(FATAL_ERROR "aiinstaller_add_package 需要 INSTALLER / MANIFEST / ROOT / OUTPUT")
  endif()

  # 产物默认放在调用方的构建目录下
  if(NOT IS_ABSOLUTE "${ARG_OUTPUT}")
    set(out_path "${CMAKE_CURRENT_BINARY_DIR}/${ARG_OUTPUT}")
  else()
    set(out_path "${ARG_OUTPUT}")
  endif()
  get_filename_component(out_dir "${out_path}" DIRECTORY)
  file(MAKE_DIRECTORY "${out_dir}")

  if(ARG_WORK_DIR)
    set(work_dir "${ARG_WORK_DIR}")
  else()
    set(work_dir "${CMAKE_CURRENT_BINARY_DIR}")
  endif()

  set(pack_args
    --installer "$<TARGET_FILE:${ARG_INSTALLER}>"
    --manifest  "${ARG_MANIFEST}"
    --root      "${ARG_ROOT}"
    --out       "${out_path}"
  )
  if(ARG_ICON)
    list(APPEND pack_args --icon "${ARG_ICON}")
  endif()

  # 清单与图标是配置期就存在的普通文件，可以直接当依赖；
  # 载荷文件是构建期产物，靠 DEPENDS 里的目标串起来。
  add_custom_command(
    OUTPUT "${out_path}"
    COMMAND aiinstaller_pack ${pack_args}
    WORKING_DIRECTORY "${work_dir}"
    DEPENDS
      "${ARG_MANIFEST}"
      ${ARG_ICON}
      aiinstaller_pack
      ${ARG_INSTALLER}
      ${ARG_DEPENDS}
    COMMENT "打包 ${ARG_OUTPUT}"
    VERBATIM
  )

  # 目标名不能用路径（CMake 不允许斜杠），去掉目录与扩展名
  get_filename_component(out_name "${out_path}" NAME_WE)
  add_custom_target("${out_name}_package" ALL DEPENDS "${out_path}")
  set_source_files_properties("${out_path}" PROPERTIES GENERATED TRUE)
endfunction()
