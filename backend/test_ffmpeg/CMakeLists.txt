cmake_minimum_required(VERSION 3.20)
project(test_ffmpeg)

set(CMAKE_CXX_STANDARD 17)

# 修改成你的实际路径
set(FFMPEG_ROOT "D:/Code/FPlayer-desketop/3rd/ffmpeg_v8.0.1")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib/mingw-64")

add_executable(test_ffmpeg main.cpp)

target_include_directories(test_ffmpeg PRIVATE
        "${FFMPEG_INCLUDE_DIR}"
)

target_link_libraries(test_ffmpeg PRIVATE
        "${FFMPEG_LIB_DIR}/libavcodec.dll.a"
        "${FFMPEG_LIB_DIR}/libavformat.dll.a"
        "${FFMPEG_LIB_DIR}/libavutil.dll.a"
)

# 可选：运行时自动复制 DLL
file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/win/*.dll")

add_custom_command(TARGET test_ffmpeg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_DLLS}
        $<TARGET_FILE_DIR:test_ffmpeg>
)