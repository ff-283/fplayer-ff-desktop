include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)
set(SPDLOG_BUILD_SHARED ON CACHE BOOL "spdlog Build Shared Lib" FORCE) # 覆盖spdlog的Option，生成静态库
# Logger
CPMAddPackage(
        NAME Logger
        GIT_REPOSITORY git@github.com:chunyujin295/Logger.git
        GIT_TAG v1.1.4
)

# ffmpeg
set(THIRD_PART_ROOT "${CMAKE_SOURCE_DIR}/3rd")
set(FFMPEG_DIR "${THIRD_PART_ROOT}/ffmpeg_v8.0.1")
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

    file(GLOB FFMPEG_DLL_FILES "${FFMPEG_DIR}/bin/win/*.dll")
    foreach (file ${FFMPEG_DLL_FILES})
        file(COPY ${file} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    endforeach ()
elseif (UNIX)
endif ()