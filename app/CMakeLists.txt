project(FPlayer_App)

config_project(ON)
add_executable(${PROJECT_NAME})
find_package(Qt6 COMPONENTS Widgets Core Gui Designer REQUIRED)

file(GLOB_RECURSE src CONFIGURE_DEPENDS *.cpp *.h)
file(GLOB_RECURSE uis CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.ui)
file(GLOB_RECURSE qrcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/*.qrc)

target_sources(${PROJECT_NAME} PRIVATE ${src} ${uis} ${qrcs})
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Gui)
target_link_libraries(${PROJECT_NAME} PRIVATE FPlayer_Common FPlayer_Widget FPlayer_Api)

#copy_imported_dependencies(${PROJECT_NAME} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

if (WIN32)
    qt_deploy_runtime(${PROJECT_NAME}) # 在build目录中对构建目标程序执行windeployqt，确保所需要Qt库和插件被正确附带
endif ()

# 设置安装项目
install(TARGETS ${PROJECT_NAME}
        #        EXPORT CxiCommonPcTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if (WIN32)
    qt_install_deploy_windeployqt(${PROJECT_NAME}) # 在安装完成的目录下对构建目标程序执行windeployqt，确保所需要Qt库和插件被正确安装
endif ()

#install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})