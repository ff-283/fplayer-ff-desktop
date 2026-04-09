project(FPlayer_Service)

find_package(Qt6 COMPONENTS Core Widgets REQUIRED)

add_standard_module(${PROJECT_NAME} include/fplayer/service ON ON)

target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Runtime FPlayer_Common)
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