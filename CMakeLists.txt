cmake_minimum_required(VERSION 3.16)
project(PhoneToolbox VERSION 1.0.0 LANGUAGES CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 自动包含当前目录
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# 查找Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

# 查找 libusb (平台相关)
if(WIN32)
    # Windows: try vcpkg first, then bundled
    if(NOT libusb-1.0_FOUND)
        find_path(LIBUSB_INCLUDE_DIRS libusb.h
            HINTS
                "${CMAKE_SOURCE_DIR}/third_party/libusb/include"
                "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
                "$ENV{VCPKG_ROOT}/packages/libusb_x64-windows/include"
        )
        find_library(LIBUSB_LIBRARIES libusb-1.0
            HINTS
                "${CMAKE_SOURCE_DIR}/third_party/libusb/lib"
                "$ENV{VCPKG_ROOT}/installed/x64-windows/lib"
                "$ENV{VCPKG_ROOT}/packages/libusb_x64-windows/lib"
        )
        if(LIBUSB_INCLUDE_DIRS AND LIBUSB_LIBRARIES)
            message(STATUS "Found libusb: ${LIBUSB_INCLUDE_DIRS}")
        else()
            # Try find_package via vcpkg
            find_package(libusb-1.0 QUIET)
            if(libusb-1.0_FOUND)
                message(STATUS "Found libusb via vcpkg")
            else()
                message(FATAL_ERROR
                    "libusb-1.0 not found. Install via:\n"
                    "  1) vcpkg: vcpkg install libusb\n"
                    "  2) or bundle headers/libs in third_party/libusb/\n"
                    "     (https://libusb.info/ -> Windows binaries)")
            endif()
        endif()
    endif()
else()
    # Linux / macOS: use pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
endif()

# 包含目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${LIBUSB_INCLUDE_DIRS}
)

# 创建源文件列表
file(GLOB_RECURSE SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp"
)

# 创建头文件列表（仅用于IDE显示）
file(GLOB_RECURSE HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.h"
)

# 打印调试信息
message(STATUS "Found sources: ${SOURCES}")
message(STATUS "Found headers: ${HEADERS}")

# 资源文件
qt_add_resources(QRC_FILES resources/resources.qrc)

# 创建可执行文件
add_executable(PhoneToolbox ${SOURCES} ${HEADERS} ${QRC_FILES})

# 平台相关链接
if(WIN32)
    target_link_libraries(PhoneToolbox
        Qt6::Core
        Qt6::Widgets
        Qt6::Network
        ${LIBUSB_LIBRARIES}
    )
else()
    target_link_libraries(PhoneToolbox
        Qt6::Core
        Qt6::Widgets
        Qt6::Network
        ${LIBUSB_LIBRARIES}
        pthread
    )
endif()

# 设置属性
if(WIN32)
    set_target_properties(PhoneToolbox PROPERTIES
        WIN32_EXECUTABLE TRUE
        MACOSX_BUNDLE FALSE
    )
else()
    set_target_properties(PhoneToolbox PROPERTIES
        WIN32_EXECUTABLE FALSE
        MACOSX_BUNDLE FALSE
    )
endif()

# Qt6 自动包含moc文件
set_target_properties(PhoneToolbox PROPERTIES
    AUTOMOC ON
)

# 安装规则（可选）
install(TARGETS PhoneToolbox DESTINATION bin)
