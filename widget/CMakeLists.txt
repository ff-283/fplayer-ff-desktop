project(FPlayer_Widget)

find_package(Qt6 COMPONENTS Widgets Core Gui Designer Network Multimedia MultimediaWidgets REQUIRED)

add_standard_module(${PROJECT_NAME} include/fplayer/widget ON ON)

target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Gui Qt6::Network Qt6::Multimedia Qt6::MultimediaWidgets)
target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Service FPlayer_Api)
target_link_libraries(${PROJECT_NAME} PRIVATE Logger)

#qt_deploy_runtime(FPlayer_Widget)

# 设置安装项目
install(TARGETS ${PROJECT_NAME}
#        EXPORT CxiCommonPcTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})