project(FPlayer_Backend_Media_Qt6)

find_package(Qt6 COMPONENTS Core Multimedia MultimediaWidgets REQUIRED)

add_standard_module(${PROJECT_NAME} include/fplayer/backend/media_qt6 ON ON)

target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Multimedia Qt6::MultimediaWidgets)
target_link_libraries(${PROJECT_NAME} PUBLIC FPlayer_Api)
target_link_libraries(${PROJECT_NAME} PRIVATE Logger)

#qt_deploy_runtime(FPlayer_Backend_Media_Qt6)
# windeployqt 主要面向 Qt 应用程序（.exe） 和 Qt 插件。
# 而你的 FPlayer_Backend_Media_Qt6 是个 普通 DLL（即使它链接了 QtCore/Gui/Network/Multimedia），它也未必包含 windeployqt 用来识别“这是 Qt 程序”的那些特征（比如 Qt 插件元数据，或主程序入口相关信息）。所以它拒绝部署，导致构建失败。
# 解决方案：只对最终的 .exe 跑 windeployqt，不要对 Core.dll 跑

# 设置安装项目
install(TARGETS ${PROJECT_NAME}
        #        EXPORT CxiCommonPcTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})