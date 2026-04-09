# \brief 宏名称 : config_project【CXI公共配置逻辑】
# @is_qt_module : 是否为Qt依赖模块 True Or False
macro(config_project is_qt_module)
    if (MSVC)
        add_compile_options("/utf-8")
        add_compile_options("-DUNICODE -D_UNICODE")
    endif ()
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if (${is_qt_module})
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOMOC ON)
        list(APPEND CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/uis")
        set(CMAKE_AUTOUIC ON)
        set(CMAKE_INCLUDE_CURRENT_DIR ON)
    endif ()
endmacro()

# \brief 宏名称    : add_standard_module【CXI添加标准模块宏】
# @target         : 库目标
# @export_location: 导出位置
# @is_shared      : 是否为动态库
# @is_qt_module   : 是否依赖Qt
macro(add_standard_module target export_location is_shared is_qt_module)
    if (${target} STREQUAL "")
        message(FATAL_ERROR "${target}目标不存在")
    endif ()
    if (NOT ${is_shared})
        message(FATAL_ERROR "动静态库配置参数不存在")
    endif ()

    config_project(${is_qt_module})
    if (${is_shared})
        add_library(${target} SHARED)
    else ()
        add_library(${target})
    endif ()
    if (NOT ${export_location} STREQUAL "")
        export_symbol(${target} ${export_location})
    endif ()
    file(GLOB_RECURSE srcs CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
    file(GLOB_RECURSE srcs_p CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/private/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/private/*.h")
    file(GLOB_RECURSE uis CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/uis/*.ui")
    file(GLOB_RECURSE qrcs CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/res/*.qrc")
    target_include_directories(${target} PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")
    target_sources(${target} PRIVATE ${srcs} ${uis} ${srcs_p} ${qrcs})
endmacro()

# ========================= 方法 =========================
# \brief 函数名称 : export_symbol【导出逻辑】
# @target      : 库目标
# @location    : 生成位置
function(export_symbol target location)
    if (NOT TARGET ${target})
        message(FATAL_ERROR "目标 ${target} 不存在，无法导出符号")
    endif ()
    if (NOT location)
        message(FATAL_ERROR "缺少导出文件路径参数")
    endif ()
    string(TOUPPER ${target} TARGET_NAME)

    set(dst "${CMAKE_CURRENT_SOURCE_DIR}/${location}/export.h")
    if (NOT EXISTS ${dst})
        configure_file("${CMAKE_SOURCE_DIR}/cmake/global_template.h.in"
                "${CMAKE_CURRENT_SOURCE_DIR}/${location}/export.h"
                @ONLY)
    endif ()
    target_compile_definitions(${target} PRIVATE "${TARGET_NAME}_LIBRARY")
endfunction()

# --------------------------------------------------
# Qt5版本的
# \brief 函数名称 : install_qt_libs【安装Qt库】
# @QT_LIBS      : Qt库列表
# @dest_dir     : 安装目录
# --------------------------------------------------
#function(install_qt_libs dest_dir)
#    set(options)
#    set(oneValueArgs)
#    set(multiValueArgs QT_LIBS)
#    cmake_parse_arguments(INSTALL_QT_LIBS
#            "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
#
#    if (WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
#        set(DEBUG_SUFFIX)
#        if (MSVC AND CMAKE_BUILD_TYPE MATCHES "Debug")
#            set(DEBUG_SUFFIX "d")
#        endif ()
#
#        # 推导 Qt 安装目录
#        set(QT_INSTALL_PATH "${Qt6_DIR}/../../../")
#        if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
#            set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
#            if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
#                set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
#            endif ()
#        endif ()
#
#        # 平台插件 (Windows 必需)
#        if (EXISTS "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll")
#            execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory
#                    "${dest_dir}/plugins/platforms/")
#            execute_process(COMMAND ${CMAKE_COMMAND} -E copy
#                    "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll"
#                    "${dest_dir}/plugins/platforms/")
#        endif ()
#
#        # Qt 库和插件目录映射（相对完整）
#        set(QT_PLUGIN_MAP
#                Gui "platforms\;imageformats\;iconengines\;platforminputcontexts\;styles"
#                Widgets "platforms\;imageformats\;iconengines\;platforminputcontexts\;styles"
#                Network "bearer"
#                Sql "sqldrivers"
#                Svg "imageformats"
#                Multimedia "mediaservice\;audio"
#                MultimediaWidgets "mediaservice\;audio"
#                Qml "qmltooling"
#                Quick "scenegraph\;qmltooling"
#                Positioning "position"
#                Sensors "sensors"
#                Bluetooth "bluetooth"
#                PrintSupport "printsupport"
#                WebEngineCore "resources\;translations"
#                WebView "resources\;translations"
#                TextToSpeech "texttospeech"
#                VirtualKeyboard "virtualkeyboard"
#        )
#
#        # 拷贝库和插件
#        foreach (QT_LIB ${INSTALL_QT_LIBS_QT_LIBS})
#            # DLL
#            execute_process(COMMAND ${CMAKE_COMMAND} -E copy
#                    "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}${DEBUG_SUFFIX}.dll"
#                    "${dest_dir}")
#
#            # 查找插件映射
#            list(FIND QT_PLUGIN_MAP ${QT_LIB} plugin_index)
#            if (NOT plugin_index EQUAL -1)
#                math(EXPR plugin_value_index "${plugin_index} + 1")
#                list(GET QT_PLUGIN_MAP ${plugin_value_index} plugin_dirs_str)
#                string(REPLACE ";" "|" plugin_dirs_pattern "${plugin_dirs_str}") # 防止 split 被误解
#                string(REPLACE ";" ";" plugin_dirs "${plugin_dirs_str}")
#
#                foreach (plugin_dir ${plugin_dirs})
#                    set(src_plugin_dir "${QT_INSTALL_PATH}/plugins/${plugin_dir}")
#                    set(dst_plugin_dir "${dest_dir}/plugins/${plugin_dir}")
#
#                    if (EXISTS "${src_plugin_dir}")
#                        execute_process(COMMAND ${CMAKE_COMMAND} -E make_directory "${dst_plugin_dir}")
#                        file(GLOB plugin_files "${src_plugin_dir}/*${DEBUG_SUFFIX}.dll")
#                        foreach (plugin_file ${plugin_files})
#                            execute_process(COMMAND ${CMAKE_COMMAND} -E copy
#                                    "${plugin_file}" "${dst_plugin_dir}")
#                        endforeach ()
#                    endif ()
#                endforeach ()
#            endif ()
#        endforeach ()
#    endif ()
#endfunction()

# ------------------------------------------------------------
# qt_deploy_runtime(<target>
#     [DEST_DIR <dir>]               # 部署目录（默认：目标exe所在目录）
#     [PLUGIN_DIR <dir>]             # 插件目录名（默认：plugins）
#     [QML_DIR <dir>]                # 如有QML可传入（可选）
#     [EXTRA_ARGS <...>]             # 额外 windeployqt 参数（可选）
# )
#
# 示例：
#   qt_deploy_runtime(FPlayer_App
#       EXTRA_ARGS --multimedia
#   )
#
#   qt_deploy_runtime(MyApp
#       DEST_DIR "${CMAKE_BINARY_DIR}/deploy/$<CONFIG>"
#       PLUGIN_DIR "plugins"
#       EXTRA_ARGS --multimedia --no-translations
#   )
# ------------------------------------------------------------
function(qt_deploy_runtime target)
    if (NOT WIN32)
        return()
    endif ()

    if (NOT TARGET ${target})
        message(FATAL_ERROR "qt_deploy_runtime: target '${target}' does not exist")
    endif ()

    set(options)
    set(oneValueArgs DEST_DIR PLUGIN_DIR QML_DIR)
    set(multiValueArgs EXTRA_ARGS)
    cmake_parse_arguments(QTDEPLOY
            "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # 默认插件输出目录
    if (NOT QTDEPLOY_PLUGIN_DIR)
        set(QTDEPLOY_PLUGIN_DIR "plugins")
    endif ()

    # 1) 定位 qmake 和 windeployqt
    get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    if (NOT _qmake_exe)
        message(FATAL_ERROR "qt_deploy_runtime: cannot locate Qt6::qmake. Did you call find_package(Qt6 ...)?")
    endif ()
    get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
    set(_windeployqt "${_qt_bin_dir}/windeployqt.exe")
    if (NOT EXISTS "${_windeployqt}")
        message(FATAL_ERROR "qt_deploy_runtime: windeployqt not found at: ${_windeployqt}")
    endif ()

    # 2) 获取 Qt 核心路径（修复：显式指定 MinGW 插件路径）
    get_target_property(_qt_core_lib Qt6::Core IMPORTED_LOCATION)
    get_filename_component(_qt_root_dir "${_qt_core_lib}" DIRECTORY)
    get_filename_component(_qt_root_dir "${_qt_root_dir}" DIRECTORY) # 上一级到 Qt 安装根目录

    # 显式指定 MinGW 平台插件路径（关键修复）
    set(_qt_platform_plugin_dir "${_qt_root_dir}/plugins/platforms")
    if (NOT EXISTS "${_qt_platform_plugin_dir}")
        message(FATAL_ERROR "Qt platforms plugin dir not found: ${_qt_platform_plugin_dir}")
    endif ()

    # 3) 部署目录配置
    set(_work_dir "$<TARGET_FILE_DIR:${target}>")
    set(_dir_arg)
    if (QTDEPLOY_DEST_DIR)
        set(_dir_arg --dir "${QTDEPLOY_DEST_DIR}")
    endif ()

    # 4) QML 配置（可选）
    set(_qml_arg)
    if (QTDEPLOY_QML_DIR)
        set(_qml_arg --qmldir "${QTDEPLOY_QML_DIR}")
    endif ()

    # 5) Debug/Release 配置
    if (MSVC)
        set(_cfg_arg $<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>)
    else ()
        set(_cfg_arg) # MinGW: 不强制 --debug/--release
    endif ()

    # 6) 修复：增强环境变量 + 显式指定平台插件路径
    # 核心改进：
    # - 同时设置 QT_PLUGIN_PATH 和 QT_QPA_PLATFORM_PLUGIN_PATH
    # - 强制包含 platforms 插件目录
    # - 增加 --verbose 便于调试
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E env
            # 双重保险：设置平台插件专用环境变量
            "QT_PLUGIN_PATH=${_qt_platform_plugin_dir};${_qt_root_dir}/plugins"
            "QT_QPA_PLATFORM_PLUGIN_PATH=${_qt_platform_plugin_dir}"
            # 执行 windeployqt，增加 --verbose 便于调试
            "${_windeployqt}"
            ${_cfg_arg}
            ${_dir_arg}
            --plugindir "${QTDEPLOY_PLUGIN_DIR}"
            --verbose 2  # 输出详细日志，便于排查问题
            --no-compiler-runtime  # 避免重复部署编译器运行时（MinGW 不需要）
            ${_qml_arg}
            ${QTDEPLOY_EXTRA_ARGS}
            "$<TARGET_FILE:${target}>"
            # 额外步骤：手动复制 platforms 插件（兜底方案）
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${_qt_platform_plugin_dir}"
            "${_work_dir}/${QTDEPLOY_PLUGIN_DIR}/platforms"
            WORKING_DIRECTORY "${_work_dir}"
            COMMENT "Deploying Qt runtime for target '${target}' via windeployqt..."
            VERBATIM
    )

    # 输出调试信息，帮助确认路径是否正确
    message(STATUS "Qt deploy config for ${target}:")
    message(STATUS "  Qt root dir: ${_qt_root_dir}")
    message(STATUS "  Platform plugin dir: ${_qt_platform_plugin_dir}")
    message(STATUS "  Target exe path: $<TARGET_FILE:${target}>")
endfunction()


# ------------------------------------------------------------
# qt_install_deploy_windeployqt(target
#    [EXTRA_ARGS ...]          # 额外参数，比如 --multimedia
#    [PLUGIN_DIR_NAME <name>]  # 默认 plugins（相对于 exe 同级）
# )
#
# 作用：
#   不负责 install target，只在 install 结束时：
#   1) 定位安装后的 exe 路径（不猜 bin，不拼路径）
#   2) cd 到 exe 所在目录
#   3) 对该 exe 运行 windeployqt，把 plugins/ 和依赖部署到 exe 同级
# ------------------------------------------------------------
function(qt_install_deploy_windeployqt target)
    if (NOT WIN32)
        return()
    endif ()
    if (NOT TARGET ${target})
        message(FATAL_ERROR "qt_install_deploy_windeployqt: target '${target}' does not exist")
    endif ()

    set(options)
    set(oneValueArgs PLUGIN_DIR_NAME)
    set(multiValueArgs EXTRA_ARGS)
    cmake_parse_arguments(QTINST
            "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT QTINST_PLUGIN_DIR_NAME)
        set(QTINST_PLUGIN_DIR_NAME "plugins")
    endif ()

    # 找 windeployqt
    get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    if (NOT _qmake_exe)
        message(FATAL_ERROR "qt_install_deploy_windeployqt: Qt6::qmake not found. Call find_package(Qt6 ...) first.")
    endif ()
    get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
    set(_windeployqt "${_qt_bin_dir}/windeployqt.exe")

    # 关键：用生成表达式得到“安装后exe路径”（不猜，不拼 bin）
    # $<TARGET_FILE_NAME:...> 只是文件名；DESTINATION 由你的 install(TARGETS) 决定
    #
    # CMake install 阶段可用变量：CMAKE_INSTALL_PREFIX、CMAKE_INSTALL_CONFIG_NAME
    # 我们不猜 DESTINATION，但仍需拿到“exe所在目录”才能 WORKING_DIRECTORY 正确。
    #
    # 最稳做法：让你显式传一个“运行目录相对路径”，但你明确说不想传/不想猜。
    # 因此我们走第二稳：基于 GNUInstallDirs 的 CMAKE_INSTALL_BINDIR，
    # 因为你自己的 install(TARGETS ...) 里已经用的是 ${CMAKE_INSTALL_BINDIR}。
    #
    # 如果你确实把 RUNTIME DESTINATION 指到了别的目录（不是 CMAKE_INSTALL_BINDIR），
    # 那就需要你把那个相对路径告诉函数（否则 CMake install 没法反推出）。
    set(_runtime_rel "${CMAKE_INSTALL_BINDIR}")

    if (MSVC)
        set(MY_CFG_ARG $<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>)
    else ()
        set(MY_CFG_ARG --release) # MinGW: 不强制 --debug/--release，但是与qt_deploy_runtime不同，
        # 下面的install文本中，不允许空的值，因此只能用release
    endif ()

    install(CODE "
        message(STATUS \"[deploy] Running windeployqt after install for target: ${target}\")

        set(_cfg \"\${CMAKE_INSTALL_CONFIG_NAME}\")
#        set(_cfg_arg \"--release\")
#        if (_cfg STREQUAL \"Debug\")
#            set(_cfg_arg \"--debug\")
#        endif()

        set(_cfg_arg \"${MY_CFG_ARG}\")

        set(_prefix \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
        set(_exe_dir \"\${_prefix}/${_runtime_rel}\")
        set(_exe \"\${_exe_dir}/$<TARGET_FILE_NAME:${target}>\")
        message(STATUS \"[deploy] exe = \${_exe}\")

        if (NOT EXISTS \"\${_exe}\")
            message(FATAL_ERROR \"[deploy] exe not found: \${_exe}\")
        endif()

        execute_process(
            COMMAND \"${_windeployqt}\"
                    \"\${_cfg_arg}\"
                    --plugindir \"${QTINST_PLUGIN_DIR_NAME}\"
                    ${QTINST_EXTRA_ARGS}
                    \"\${_exe}\"
            WORKING_DIRECTORY \"\${_exe_dir}\"
            RESULT_VARIABLE _ret
        )
        if (NOT _ret EQUAL 0)
            message(FATAL_ERROR \"[deploy] windeployqt failed: \${_ret}\")
        endif()
    ")
endfunction()