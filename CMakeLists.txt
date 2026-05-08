cmake_minimum_required(VERSION 3.15)
project(FPlayer_Desketop)

set(CMAKE_CXX_STANDARD 17)
include(GNUInstallDirs)
set(FPLAYER_PACKAGE_VERSION "0.1.0" CACHE STRING "Package version for CPack")

set(CMAKE_DEBUG_POSTFIX d)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(cmake/3rd.cmake)
include(cmake/utils.cmake)

# MinGW下静态链接通用依赖库
if (MINGW)
    add_link_options(
            -static-libgcc
            -static-libstdc++
            -Wl,-Bstatic
            -lwinpthread
            -Wl,-Bdynamic
    )
endif()

if (UNIX AND NOT APPLE)
    # Linux 下用 RPATH（而非 RUNPATH）以支持间接依赖库的可传递查找，
    # 避免 FFmpeg so 链式依赖在启动时出现 "not found"。
    add_link_options(-Wl,--disable-new-dtags)
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

add_subdirectory(common)
add_subdirectory(api)
add_subdirectory(backend)
add_subdirectory(runtime)
add_subdirectory(service)
add_subdirectory(widget)
add_subdirectory(app)

if (UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_PACKAGE_NAME "fplayer-desktop")
    set(CPACK_PACKAGE_VENDOR "fplayer")
    set(CPACK_PACKAGE_CONTACT "fplayer@local")
    set(CPACK_PACKAGE_VERSION "${FPLAYER_PACKAGE_VERSION}")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "fplayer")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()

if (WIN32 AND MSVC)
    set(FPLAYER_WINDOWS_ICON "${CMAKE_SOURCE_DIR}/app/res/icon/icon.ico")

    # Use both generators: NSIS for setup.exe and WIX for msi.
    set(CPACK_GENERATOR "NSIS;WIX")

    set(CPACK_PACKAGE_NAME "FPlayer Desktop")
    set(CPACK_PACKAGE_VENDOR "fplayer")
    set(CPACK_PACKAGE_CONTACT "fplayer@local")
    set(CPACK_PACKAGE_VERSION "${FPLAYER_PACKAGE_VERSION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "FPlayerDesktop")

    # NSIS metadata.
    set(CPACK_NSIS_PACKAGE_NAME "FPlayer Desktop")
    set(CPACK_NSIS_DISPLAY_NAME "FPlayer Desktop")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MUI_ICON "${FPLAYER_WINDOWS_ICON}")
    set(CPACK_NSIS_MUI_UNIICON "${FPLAYER_WINDOWS_ICON}")

    # WIX metadata.
    # Upgrade GUID should stay stable across versions to enable upgrades.
    set(CPACK_WIX_UPGRADE_GUID "659644c7-7eca-4dbf-8884-682d0c6b761c")
    # Auto-generate product GUID for each build.
    set(CPACK_WIX_PRODUCT_GUID "*")
    set(CPACK_WIX_PRODUCT_ICON "${FPLAYER_WINDOWS_ICON}")

    # ---- VC++ Redistributable (VCRUNTIME140.dll, MSVCP140.dll, etc.) ----
    # Find or download vc_redist.x64.exe so target machines don't show "missing VCRUNTIME140.dll".
    set(_vcredist_exe "")

    # Helper: check downloaded file is non-empty, delete if broken
    macro(_verify_vcredist _path)
        if(EXISTS "${_path}")
            file(SIZE "${_path}" _vcs)
            if(_vcs GREATER 0)
                set(_vcredist_exe "${_path}")
            else()
                message(WARNING "Removing zero-byte vc_redist.x64.exe: ${_path}")
                file(REMOVE "${_path}")
            endif()
        endif()
    endmacro()

    # 1) Pre-downloaded copy in cmake/ or disk/vcredist (offline / CI cache)
    _verify_vcredist("${CMAKE_SOURCE_DIR}/cmake/vc_redist.x64.exe")
    if(_vcredist_exe)
        file(COPY "${_vcredist_exe}" DESTINATION "${CMAKE_SOURCE_DIR}/disk/vcredist")
    else()
        _verify_vcredist("${CMAKE_SOURCE_DIR}/disk/vcredist/vc_redist.x64.exe")
    endif()

    # 2) Search Visual Studio 2022 installation
    if(NOT _vcredist_exe)
        foreach(_edition Community Professional Enterprise BuildTools)
            file(GLOB _vcredist_candidates
                "C:/Program Files/Microsoft Visual Studio/2022/${_edition}/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT/vc_redist.x64.exe"
            )
            if(_vcredist_candidates)
                list(GET _vcredist_candidates 0 _vcredist_exe)
                break()
            endif()
        endforeach()
    endif()

    # 3) Download from Microsoft (aka.ms redirects to latest VC++ 2022 redist)
    if(NOT _vcredist_exe)
        set(_vcredist_dl_dir "${CMAKE_BINARY_DIR}/vcredist")
        set(_vcredist_exe "${_vcredist_dl_dir}/vc_redist.x64.exe")
        _verify_vcredist("${_vcredist_exe}")  # re-check after possible failed download
        if(NOT _vcredist_exe)
            file(MAKE_DIRECTORY "${_vcredist_dl_dir}")
            message(STATUS "Downloading VC++ Redistributable from Microsoft...")
            file(DOWNLOAD
                "https://aka.ms/vs/17/release/vc_redist.x64.exe"
                "${_vcredist_exe}"
                STATUS _vcredist_dl_status
                TLS_VERIFY ON
                TIMEOUT 120
            )
            list(GET _vcredist_dl_status 0 _vcredist_dl_code)
            if(NOT _vcredist_dl_code EQUAL 0)
                list(GET _vcredist_dl_status 1 _vcredist_dl_error)
                message(WARNING "Failed to download VC++ Redistributable: ${_vcredist_dl_error}")
                message(WARNING "Installers will NOT include the VC++ runtime.")
                message(WARNING "Download it manually and place at: ${_vcredist_exe}")
                message(WARNING "  https://aka.ms/vs/17/release/vc_redist.x64.exe")
                set(_vcredist_exe "")
            else()
                _verify_vcredist("${_vcredist_exe}")
                if(_vcredist_exe)
                    message(STATUS "Downloaded VC++ Redistributable successfully.")
                else()
                    message(WARNING "Download completed but file is empty. Place manually at disk/vcredist/")
                endif()
            endif()
        endif()
    endif()

    if(_vcredist_exe AND EXISTS "${_vcredist_exe}")
        message(STATUS "Bundling VC++ Redistributable: ${_vcredist_exe}")

        # Ship the redist installer inside the package
        install(
            FILES "${_vcredist_exe}"
            DESTINATION "${CMAKE_INSTALL_BINDIR}"
        )

        # NSIS: run redist silently after files are laid down, then delete it.
        # We use CPACK_PROJECT_CONFIG_FILE (an extra CMake file included by
        # CPackConfig.cmake at CPack time) to set the NSIS commands via a
        # bracket argument.  This avoids the double-quote escaping problem
        # that would happen if the commands went through CPackConfig.cmake's
        # own double-quoted set().
        file(WRITE "${CMAKE_BINARY_DIR}/CPackExtra.cmake" [=[
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS [==[
DetailPrint "Installing Visual C++ Redistributable..."
ExecWait '"$INSTDIR\bin\vc_redist.x64.exe" /install /quiet /norestart' $0
DetailPrint "VC++ Redistributable exit code: $0"
Delete "$INSTDIR\bin\vc_redist.x64.exe"
]==])
]=])
        set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_BINARY_DIR}/CPackExtra.cmake")
    else()
        message(WARNING "VC++ Redistributable not available.")
        message(WARNING "Target machines may see \"VCRUNTIME140.dll not found\".")
        message(WARNING "Users must install the VC++ runtime manually:")
        message(WARNING "  https://aka.ms/vs/17/release/vc_redist.x64.exe")
    endif()
endif()
include(CPack)




