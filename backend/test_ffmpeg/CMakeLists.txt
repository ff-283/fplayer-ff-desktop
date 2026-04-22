cmake_minimum_required(VERSION 3.20)
project(test_ffmpeg)

set(CMAKE_CXX_STANDARD 17)

# 修改成你的实际路径
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/3rd/ffmpeg_v8.1")
if (NOT EXISTS "${FFMPEG_ROOT}")
    set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/3rd/ffmpeg_v8.0.1")
endif ()
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
if (WIN32)
    set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib/mingw-64")
elseif (UNIX)
    set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib/gcc-lib64")
endif ()

add_executable(test_ffmpeg main.cpp)

target_include_directories(test_ffmpeg PRIVATE
        "${FFMPEG_INCLUDE_DIR}"
)

if (WIN32)
    target_link_libraries(test_ffmpeg PRIVATE
            "${FFMPEG_LIB_DIR}/libavcodec.dll.a"
            "${FFMPEG_LIB_DIR}/libavformat.dll.a"
            "${FFMPEG_LIB_DIR}/libavutil.dll.a"
    )

    # 可选：运行时自动复制 DLL
    file(GLOB FFMPEG_RUNTIME_LIBS "${FFMPEG_ROOT}/bin/win/*.dll")
elseif (UNIX)
    target_link_directories(test_ffmpeg PRIVATE "${FFMPEG_LIB_DIR}")
    target_link_libraries(test_ffmpeg PRIVATE avcodec avformat avutil)
    file(GLOB FFMPEG_RUNTIME_LIBS
            "${FFMPEG_LIB_DIR}/libavcodec.so*"
            "${FFMPEG_LIB_DIR}/libavformat.so*"
            "${FFMPEG_LIB_DIR}/libavutil.so*")
    set_target_properties(test_ffmpeg PROPERTIES
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
endif ()

if (FFMPEG_RUNTIME_LIBS)
    add_custom_command(TARGET test_ffmpeg POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_RUNTIME_LIBS}
            $<TARGET_FILE_DIR:test_ffmpeg>
    )
endif ()