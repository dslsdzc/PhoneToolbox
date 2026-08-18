# 华为刷写插件：独立 .so，主程序不链接（法务/许可隔离）
find_package(Qt6 REQUIRED COMPONENTS Core)
add_library(huawei_flash_plugin MODULE
    huawei_flash_plugin.h
    huawei_flash_plugin.cpp
    update_app.h
    update_app.cpp
    hisi_flash.h
    hisi_flash.cpp
    hisi_update.h
    hisi_update.cpp)
target_include_directories(huawei_flash_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}  # 引用 src/plugins/plugin_interface.h
    ${LIBUSB_INCLUDE_DIRS})  # hisi_update.cpp 的 libusb.h
target_link_libraries(huawei_flash_plugin PRIVATE
    Qt6::Core ${LIBUSB_LIBRARIES} ${ZLIB_LIBRARY})
set_target_properties(huawei_flash_plugin PROPERTIES
    AUTOMOC ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins)

# 协议层测试（插件内独立注册，不进入主项目测试列表）
find_package(Qt6 QUIET COMPONENTS Test)
if(Qt6Test_FOUND AND ENABLE_IMAGE_TESTS)
    enable_testing()
    add_executable(test_hisi_update
        tests/test_hisi_update.cpp
        hisi_update.cpp
        hisi_flash.cpp
        update_app.cpp)
    target_include_directories(test_hisi_update PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${LIBUSB_INCLUDE_DIRS})
    target_link_libraries(test_hisi_update PRIVATE Qt6::Test ${LIBUSB_LIBRARIES} ${ZLIB_LIBRARY})
    set_target_properties(test_hisi_update PROPERTIES AUTOMOC ON)
    add_test(NAME test_hisi_update COMMAND test_hisi_update)
endif()
