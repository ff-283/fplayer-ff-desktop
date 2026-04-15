project(FPlayer_Backend_Desktop_Capture_Dxgi)

find_package(Qt6 COMPONENTS Core Gui REQUIRED)

add_standard_module(${PROJECT_NAME} include/fplayer/backend/desktopcapture_dxgi ON ON)

target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Gui)
target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Api)
target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Common)
target_link_libraries(${PROJECT_NAME} PRIVATE Logger)
target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE swscale avutil)
target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_HEADER})

if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE d3d11 dxgi)
endif ()

install(TARGETS ${PROJECT_NAME}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
