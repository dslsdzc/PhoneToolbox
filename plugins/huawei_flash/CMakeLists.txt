# 华为刷写插件：独立 .so，主程序不链接（法务/许可隔离）
find_package(Qt6 REQUIRED COMPONENTS Core)
add_library(huawei_flash_plugin MODULE
    huawei_flash_plugin.h)
target_include_directories(huawei_flash_plugin PRIVATE
    ${CMAKE_SOURCE_DIR})  # 引用 src/plugins/plugin_interface.h
target_link_libraries(huawei_flash_plugin PRIVATE Qt6::Core)
set_target_properties(huawei_flash_plugin PROPERTIES
    AUTOMOC ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins)
