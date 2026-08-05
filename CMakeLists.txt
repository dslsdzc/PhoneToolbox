cmake_minimum_required(VERSION 3.16)
project(PhoneToolbox VERSION 2.0.0 LANGUAGES CXX)

# 选项：静态构建（Windows 单 EXE）
option(STATIC_BUILD "Build fully static executable (single EXE on Windows)" OFF)

if(STATIC_BUILD AND WIN32)
    message(STATUS "Static build enabled - targeting single EXE")
    # 优先找静态库 (.lib)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
    # Qt 静态插件标记
    add_compile_definitions(QT_STATIC)
    # 优先使用 vcpkg static triplet
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_PREFIX_PATH "$ENV{VCPKG_ROOT}/installed/x64-windows-static"
            CACHE PATH "vcpkg static triplet prefix")
    endif()
endif()

# C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

# 查找 Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

# 查找 libusb（平台相关）
if(WIN32)
    if(STATIC_BUILD)
        # 静态构建：找 libusb-1.0.lib
        find_path(LIBUSB_INCLUDE_DIRS libusb.h
            HINTS
                "${CMAKE_SOURCE_DIR}/third_party/libusb/include"
                "$ENV{VCPKG_ROOT}/installed/x64-windows-static/include"
        )
        find_library(LIBUSB_LIBRARIES usb-1.0
            HINTS
                "${CMAKE_SOURCE_DIR}/third_party/libusb/lib"
                "$ENV{VCPKG_ROOT}/installed/x64-windows-static/lib"
        )
        if(LIBUSB_INCLUDE_DIRS AND LIBUSB_LIBRARIES)
            message(STATUS "Found static libusb: ${LIBUSB_INCLUDE_DIRS}")
        else()
            message(FATAL_ERROR
                "Static libusb-1.0 not found. Install via:\n"
                "  vcpkg install libusb:x64-windows-static")
        endif()
    else()
        # 动态构建：找 libusb-1.0.dll import lib
        find_path(LIBUSB_INCLUDE_DIRS libusb.h
            HINTS
                "${CMAKE_SOURCE_DIR}/third_party/libusb/include"
                "$ENV{VCPKG_ROOT}/installed/x64-windows/include"
        )
        find_library(LIBUSB_LIBRARIES libusb-1.0
            HINTS
                "${CMAKE_SOURCE_DIR}/third_party/libusb/lib"
                "$ENV{VCPKG_ROOT}/installed/x64-windows/lib"
        )
        if(NOT LIBUSB_INCLUDE_DIRS OR NOT LIBUSB_LIBRARIES)
            find_package(libusb-1.0 QUIET)
            if(NOT libusb-1.0_FOUND)
                message(FATAL_ERROR
                    "libusb-1.0 not found. Install via:\n"
                    "  1) vcpkg: vcpkg install libusb\n"
                    "  2) bundle in third_party/libusb/")
            endif()
        endif()
    endif()
else()
    # Linux / macOS: pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
endif()

# 压缩库（镜像处理: payload/zstd/lz4/bzip2/xz/brotli）
find_library(ZSTD_LIBRARY zstd)
find_path(ZSTD_INCLUDE_DIR zstd.h)
find_library(LZ4_LIBRARY lz4)
find_path(LZ4_INCLUDE_DIR lz4frame.h)
find_library(BZIP2_LIBRARY bz2)
find_path(BZIP2_INCLUDE_DIR bzlib.h)
find_library(XZ_LIBRARY lzma)
find_path(XZ_INCLUDE_DIR lzma.h)
find_library(BROTLI_LIBRARY brotlidec)
find_library(BROTLIENC_LIBRARY brotlienc)
find_path(BROTLI_INCLUDE_DIR brotli/decode.h)
# zlib（gzip 封装）：Qt6::Core 依赖 zlib 但符号不可见，显式链接（与 zstd/lz4/bz2/xz 一致）。
# 多名字匹配：Linux 为 libz.so，vcpkg Windows 产出 zlib.lib（而非 z.lib）
find_library(ZLIB_LIBRARY NAMES z zlib zlib1 zlibstatic)
if(NOT ZSTD_LIBRARY OR NOT LZ4_LIBRARY OR NOT BZIP2_LIBRARY OR NOT XZ_LIBRARY OR NOT BROTLI_LIBRARY OR NOT BROTLIENC_LIBRARY OR NOT ZLIB_LIBRARY)
    message(FATAL_ERROR "Compression libs not found. Linux: pacman -S zstd lz4 bzip2 xz brotli zlib  |  Windows: vcpkg install zstd lz4 bzip2 xz brotli zlib")
endif()

# 包含目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${LIBUSB_INCLUDE_DIRS}
)

# 源文件列表
file(GLOB_RECURSE SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp"
)

# image_engine 与 root_patcher 由独立静态库目标编译，从主程序 GLOB 中排除
list(FILTER SOURCES EXCLUDE REGEX "/image_engine/")
list(FILTER SOURCES EXCLUDE REGEX "/root_patcher/")

file(GLOB_RECURSE HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.h"
)
# HEADERS 同样必须过滤：root_patcher 的 Q_OBJECT 头若进主程序 AUTOMOC，
# 会生成引用未链接构造函数的 moc，导致主程序链接失败
list(FILTER HEADERS EXCLUDE REGEX "/image_engine/")
list(FILTER HEADERS EXCLUDE REGEX "/root_patcher/")

message(STATUS "Found sources: ${SOURCES}")
message(STATUS "Found headers: ${HEADERS}")

# 资源文件
qt_add_resources(QRC_FILES resources/resources.qrc)

# 创建可执行文件
add_executable(PhoneToolbox ${SOURCES} ${HEADERS} ${QRC_FILES})

# Qt 静态插件（Windows 单 EXE 必需）
if(STATIC_BUILD AND WIN32)
    qt_import_plugins(PhoneToolbox
        INCLUDE
            Qt::QWindowsIntegrationPlugin
            Qt::QWindowsVistaStylePlugin
    )
endif()

# 链接库
if(WIN32)
    target_link_libraries(PhoneToolbox
        Qt6::Core
        Qt6::Widgets
        Qt6::Network
        ${LIBUSB_LIBRARIES}
    )
    if(STATIC_BUILD)
        # 静态 Qt 需要显式链接系统库
        target_link_libraries(PhoneToolbox
            winmm
            ws2_32
            user32
            gdi32
            shell32
            ole32
            oleaut32
            comdlg32
            advapi32
            crypt32
            imm32
        )
    endif()
else()
    target_link_libraries(PhoneToolbox
        Qt6::Core
        Qt6::Widgets
        Qt6::Network
        ${LIBUSB_LIBRARIES}
        pthread
    )
endif()

# 可执行文件属性
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

# AUTOMOC
set_target_properties(PhoneToolbox PROPERTIES
    AUTOMOC ON
)

# 安装规则
install(TARGETS PhoneToolbox DESTINATION bin)

# ---- image_engine 静态库 ----
file(GLOB_RECURSE IMAGE_ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/image_engine/*.cpp")
file(GLOB_RECURSE IMAGE_ENGINE_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/image_engine/*.h")
add_library(image_engine STATIC ${IMAGE_ENGINE_SOURCES} ${IMAGE_ENGINE_HEADERS})
target_include_directories(image_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ZSTD_INCLUDE_DIR} ${LZ4_INCLUDE_DIR} ${BZIP2_INCLUDE_DIR}
    ${XZ_INCLUDE_DIR} ${BROTLI_INCLUDE_DIR})
target_link_libraries(image_engine PUBLIC Qt6::Core
    ${ZSTD_LIBRARY} ${LZ4_LIBRARY} ${BZIP2_LIBRARY} ${XZ_LIBRARY}
    ${BROTLI_LIBRARY} ${BROTLIENC_LIBRARY} ${ZLIB_LIBRARY})
set_target_properties(image_engine PROPERTIES AUTOMOC ON)

# 主程序链接 image_engine
target_link_libraries(PhoneToolbox image_engine)

# 主程序链接 root_patcher（D1 Minor 2 吸收：D4 修补接线需要 patcher::patchFile；
# root_patcher PUBLIC 传递 image_engine，顺序无关）
target_link_libraries(PhoneToolbox root_patcher)

# ---- root_patcher 静态库 ----
file(GLOB_RECURSE ROOT_PATCHER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/root_patcher/*.cpp")
file(GLOB_RECURSE ROOT_PATCHER_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/root_patcher/*.h")
add_library(root_patcher STATIC ${ROOT_PATCHER_SOURCES} ${ROOT_PATCHER_HEADERS})
target_include_directories(root_patcher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# lz4/lzma 头与库经 image_engine PUBLIC 传递；Qt6::Network 供后续下载类使用
target_link_libraries(root_patcher PUBLIC image_engine Qt6::Core Qt6::Network)
set_target_properties(root_patcher PROPERTIES AUTOMOC ON)

# ---- root_patcher 单元测试 ----
option(ENABLE_ROOT_PATCHER_TESTS "Build root_patcher unit tests" ON)
if(ENABLE_ROOT_PATCHER_TESTS)
    find_package(Qt6 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        set(ROOT_PATCHER_TEST_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_patcher.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ramdisk.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_downloader.cpp)
        # 与 image_engine 测试一致：每个测试源独立可执行文件（首源沿用 root_patcher_tests 目标名）
        set(_test_count 0)
        foreach(_test_src IN LISTS ROOT_PATCHER_TEST_SOURCES)
            if(_test_count EQUAL 0)
                set(_test_target root_patcher_tests)
            else()
                get_filename_component(_test_stem ${_test_src} NAME_WE)
                set(_test_target root_patcher_tests_${_test_stem})
            endif()
            math(EXPR _test_count "${_test_count} + 1")
            add_executable(${_test_target} ${_test_src})
            target_link_libraries(${_test_target} PRIVATE root_patcher image_engine Qt6::Test)
            set_target_properties(${_test_target} PROPERTIES AUTOMOC ON)
            add_test(NAME ${_test_target} COMMAND ${_test_target})
        endforeach()
    endif()
endif()

# ---- image_engine 单元测试 ----
option(ENABLE_IMAGE_TESTS "Build image_engine unit tests" ON)
if(ENABLE_IMAGE_TESTS)
    find_package(Qt6 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        set(IMAGE_TEST_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compression.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sparse.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_boot.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tar.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_payload.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bspatch.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dat.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_registry.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_super.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_twrp.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_disk.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_kdz.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_huawei.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sin.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pac.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fs_erofs.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fs_ext4.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_engineer_mode.cpp)
        if(IMAGE_TEST_SOURCES)
            # QTEST_APPLESS_MAIN 在每个测试文件里自带 main，多个测试源无法合链进同一可执行文件，
            # 故每个测试源独立编译为一个可执行文件（首源沿用 image_engine_tests 目标名）。
            set(_test_count 0)
            # 首轮循环走 if(_test_count EQUAL 0) 分支，不会经 get_filename_component
            # 赋值 _test_stem —— 循环前显式置空，避免读未初始化变量/残留值。
            set(_test_stem "")
            foreach(_test_src IN LISTS IMAGE_TEST_SOURCES)
                if(_test_count EQUAL 0)
                    set(_test_target image_engine_tests)
                else()
                    get_filename_component(_test_stem ${_test_src} NAME_WE)
                    set(_test_target image_engine_tests_${_test_stem})
                endif()
                math(EXPR _test_count "${_test_count} + 1")
                # 核心模块测试：被测源在 src/core（已 GLOB 编入主程序，但不属于任何静态库），
                # 把被测源文件直接编入该测试可执行文件。
                set(_test_extra_sources "")
                if(_test_stem STREQUAL "test_engineer_mode")
                    set(_test_extra_sources ${CMAKE_CURRENT_SOURCE_DIR}/src/core/engineer_mode.cpp)
                endif()
                add_executable(${_test_target} ${_test_src} ${_test_extra_sources})
                target_link_libraries(${_test_target} PRIVATE image_engine Qt6::Test)
                set_target_properties(${_test_target} PROPERTIES AUTOMOC ON)
                add_test(NAME ${_test_target} COMMAND ${_test_target})
            endforeach()
        endif()
    endif()
endif()
