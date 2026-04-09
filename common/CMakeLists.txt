project(FPlayer_Common)

find_package(Qt6 COMPONENTS Core OpenGL OpenGLWidgets REQUIRED)

add_standard_module(${PROJECT_NAME} include/fplayer/common ON ON)

target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::OpenGL Qt6::OpenGLWidgets)
target_link_libraries(${PROJECT_NAME} PUBLIC Qt6::OpenGL Qt6::OpenGLWidgets)
target_link_libraries(${PROJECT_NAME} PUBLIC Logger)
# 只要一个头文件会被别人 include，它依赖的 include 路径/库就必须通过 PUBLIC/INTERFACE 传递给使用者
# 现在common会被sample调用，所以需要将Logger设为公共的

# 设置安装项目
install(TARGETS ${PROJECT_NAME}
        #        EXPORT CxiCommonPcTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})