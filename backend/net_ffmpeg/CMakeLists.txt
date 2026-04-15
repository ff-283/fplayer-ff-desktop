project(FPlayer_Backend_Net_FFmpeg)

find_package(Qt6 COMPONENTS Core REQUIRED)

add_standard_module(${PROJECT_NAME} include/fplayer/backend/net_ffmpeg ON ON)

target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core)
target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Api)
target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Common)
target_link_libraries(${PROJECT_NAME} PRIVATE Logger)
target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE avdevice avformat avcodec swscale swresample avutil)
target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_HEADER})

install(TARGETS ${PROJECT_NAME}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
