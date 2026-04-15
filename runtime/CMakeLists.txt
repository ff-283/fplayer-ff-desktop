project(FPlayer_Runtime)

find_package(Qt6 COMPONENTS Core REQUIRED)

add_standard_module(${PROJECT_NAME} include/fplayer/runtime ON ON)

target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core)
if(FPLAYER_BUILD_MEDIA_QT6)
    target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Backend_Media_Qt6)
endif()

if(FPLAYER_BUILD_MEDIA_FFMPEG)
    target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Backend_Media_FFmpeg)
endif()

if(FPLAYER_BUILD_MEDIA_DXGI AND WIN32)
    target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Backend_Desktop_Capture_Dxgi)
    target_compile_definitions(${PROJECT_NAME} PUBLIC FPLAYER_WITH_SCREEN_DXGI=1)
endif()

if(FPLAYER_BUILD_NET_FFMPEG)
    target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Backend_Net_FFmpeg)
    target_compile_definitions(${PROJECT_NAME} PUBLIC FPLAYER_WITH_NET_FFMPEG=1)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE Logger)


# 设置安装项目
install(TARGETS ${PROJECT_NAME}
        #        EXPORT CxiCommonPcTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})