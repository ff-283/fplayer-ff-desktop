project(FPlayer_App)

config_project(ON)
if (WIN32)
    add_executable(${PROJECT_NAME} WIN32)
else()
    add_executable(${PROJECT_NAME})
endif()
find_package(Qt6 COMPONENTS Widgets Core Gui Designer REQUIRED)

file(GLOB_RECURSE src CONFIGURE_DEPENDS *.cpp *.h)

target_sources(${PROJECT_NAME} PRIVATE ${src})
if (WIN32)
    target_sources(${PROJECT_NAME} PRIVATE app_icon.rc)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Gui)
target_link_libraries(${PROJECT_NAME} PRIVATE FPlayer_Common FPlayer_Widget FPlayer_Api)


if (WIN32)
    qt_deploy_runtime(${PROJECT_NAME}) # 在build目录中对构建目标程序执行windeployqt，确保所需要Qt库和插件被正确附带
endif ()

# 设置安装项目
install(TARGETS ${PROJECT_NAME}
	        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	)

if (WIN32)
    qt_install_deploy_windeployqt(${PROJECT_NAME}) # 在安装完成的目录下对构建目标程序执行windeployqt，确保所需要Qt库和插件被正确安装
endif ()

