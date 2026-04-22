project(FPlayer_Backend_Stream_FFmpeg)

find_package(Qt6 COMPONENTS Core Multimedia REQUIRED)

add_standard_module(${PROJECT_NAME} include/fplayer/backend/stream_ffmpeg ON ON)

target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Multimedia)
target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Api)
target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Common)
target_link_libraries(${PROJECT_NAME} PRIVATE Logger)
target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE
        avdevice
        avfilter
        avformat avcodec swscale swresample avutil)
if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ksuser uuid)# mingw下需要
endif()
target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_HEADER})

install(TARGETS ${PROJECT_NAME}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
