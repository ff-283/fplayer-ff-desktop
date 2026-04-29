cmake_minimum_required(VERSION 3.15)
project(FPlayer_Desketop)

set(CMAKE_CXX_STANDARD 17)
include(GNUInstallDirs)

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
    set(CPACK_PACKAGE_VERSION "0.1.0")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "fplayer")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()

if (WIN32 AND MSVC)
    set(FPLAYER_WINDOWS_ICON "${CMAKE_SOURCE_DIR}/app/res/icon/LOGO_ICON_CUTED.ico")

    # Use both generators: NSIS for setup.exe and WIX for msi.
    set(CPACK_GENERATOR "NSIS;WIX")

    set(CPACK_PACKAGE_NAME "FPlayer Desktop")
    set(CPACK_PACKAGE_VENDOR "fplayer")
    set(CPACK_PACKAGE_CONTACT "fplayer@local")
    set(CPACK_PACKAGE_VERSION "0.1.0")
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
endif()
include(CPack)




