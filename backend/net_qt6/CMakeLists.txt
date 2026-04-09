project(FPlayer_Backend_Net_Qt6)

find_package(Qt6 COMPONENTS Core Network REQUIRED)

add_standard_module(${PROJECT_NAME} include/fplayer/backend/net_qt6 ON ON)

target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Network)
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