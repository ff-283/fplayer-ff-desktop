include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)
set(SPDLOG_BUILD_SHARED ON CACHE BOOL "spdlog Build Shared Lib" FORCE) # 覆盖spdlog的Option，生成静态库
# Logger
set(FPLAYER_LOGGER_REPO "git@github.com:chunyujin295/Logger.git")
if (UNIX AND NOT APPLE)
    # Linux CI/构建机上通常没有 SSH key，优先走 HTTPS 拉取依赖。
    set(FPLAYER_LOGGER_REPO "https://github.com/chunyujin295/Logger.git")
endif ()
CPMAddPackage(
        NAME Logger
        GIT_REPOSITORY ${FPLAYER_LOGGER_REPO}
        GIT_TAG v1.1.4
)

# ffmpeg
set(THIRD_PART_ROOT "${CMAKE_SOURCE_DIR}/3rd")
set(FFMPEG_DIR "${THIRD_PART_ROOT}/ffmpeg_v8.1")
if (NOT EXISTS "${FFMPEG_DIR}")
    set(FFMPEG_DIR "${THIRD_PART_ROOT}/ffmpeg_v8.0.1")
endif ()
set(FFMPEG_HEADER "${FFMPEG_DIR}/include")

# 拷贝ffmepg库到build/bin下
if (WIN32)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        message(STATUS "Using MSVC")
        set(FFMPEG_LIBRARIES "${FFMPEG_DIR}/lib/msvc-64")
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(STATUS "Using MinGW (GNU)")
        set(FFMPEG_LIBRARIES "${FFMPEG_DIR}/lib/mingw-64")
    endif ()

    file(GLOB FFMPEG_RUNTIME_FILES "${FFMPEG_DIR}/bin/win/*.dll")
    foreach (file ${FFMPEG_RUNTIME_FILES})
        file(COPY ${file} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    endforeach ()
elseif (UNIX)
    set(FFMPEG_LIBRARIES "${FFMPEG_DIR}/lib/gcc-lib64")
    file(GLOB FFMPEG_RUNTIME_FILES
            "${FFMPEG_LIBRARIES}/*.so*")

    foreach (file ${FFMPEG_RUNTIME_FILES})
        file(COPY ${file} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    endforeach ()
endif ()

if (FFMPEG_RUNTIME_FILES)
    if (WIN32)
        install(FILES ${FFMPEG_RUNTIME_FILES} DESTINATION ${CMAKE_INSTALL_BINDIR})
    elseif (UNIX)
        install(FILES ${FFMPEG_RUNTIME_FILES} DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif ()
endif ()