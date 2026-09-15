cmake_minimum_required(VERSION 3.16)
project(PhoneToolbox VERSION 0.0.1 LANGUAGES CXX)

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
# src/plugins 同理：由 phone_plugins 静态库编译，主程序仅链接（避免双重编译）
list(FILTER SOURCES EXCLUDE REGEX "/image_engine/")
list(FILTER SOURCES EXCLUDE REGEX "/root_patcher/")
list(FILTER SOURCES EXCLUDE REGEX "/src/plugins/")

file(GLOB_RECURSE HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.h"
)
# HEADERS 同样必须过滤：root_patcher 的 Q_OBJECT 头若进主程序 AUTOMOC，
# 会生成引用未链接构造函数的 moc，导致主程序链接失败
list(FILTER HEADERS EXCLUDE REGEX "/image_engine/")
list(FILTER HEADERS EXCLUDE REGEX "/root_patcher/")
list(FILTER HEADERS EXCLUDE REGEX "/src/plugins/")

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
# ON = MTK 真样本缺失/不足时让用例 FAIL 而非 SKIP（CI/验证跑用；默认 OFF 保持离线友好）
option(MTK_SAMPLES_REQUIRED "Fail (instead of skip) when MTK samples are missing" OFF)
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
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_engineer_mode.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resource_monitor.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mtk_brom.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mtk_payload.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spd_flash.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pipeline.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_oppo_crypto.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_oppo_keys.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_oppo_ofp.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_oppo_extract.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_oppo_ops.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_image_worker.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_flash_plan.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_edl_sahara.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_edl_firehose.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_edl_libusb_transport.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_edl_session.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_flash_plan_dialog.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pit.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_samsung_plan.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_odin_protocol.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_odin_session.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_odin_libusb_transport.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_samsung_plan_dialog.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mtk_da_file.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mtk_chip_table.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mtk_preloader_emi.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mtk_flash_plan.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mtk_preloader_fetch.cpp)
        if(IMAGE_TEST_SOURCES)
            # QTEST_APPLESS_MAIN 在每个测试文件里自带 main，多个测试源无法合链进同一可执行文件，
            # 故每个测试源独立编译为一个可执行文件（首源沿用 image_engine_tests 目标名）。
            set(_test_count 0)
            # 首轮循环走 if(_test_count EQUAL 0) 分支，不会经 get_filename_component
            # 赋值 _test_stem —— 循环前显式置空，避免读未初始化变量/残留值。
            set(_test_stem "")
            # 三星真样本目录（reference/ 为 gitignored）：只把**路径**编进用例，内容不进仓库。
            # PT_ODIN_SAMPLES_DIR 是 CACHE 变量 —— 便于做"指向不存在目录"的负控制实验。
            set(PT_ODIN_SAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/reference/samsung-samples"
                CACHE PATH "三星真样本目录（gitignored）")
            # ON = 真样本缺失/不足时让用例 FAIL 而非 SKIP（本仓 CI/验证跑这个；默认 OFF 保持离线友好）
            option(ODIN_SAMPLES_REQUIRED "Fail (instead of skip) when Samsung samples are missing" OFF)
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
                elseif(_test_stem STREQUAL "test_resource_monitor")
                    set(_test_extra_sources ${CMAKE_CURRENT_SOURCE_DIR}/src/core/resource_monitor.cpp)
                elseif(_test_stem STREQUAL "test_mtk_brom")
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_brom.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_emmc.cpp)
                elseif(_test_stem STREQUAL "test_mtk_payload")
                    # D1-T6：夹具 makeSelection() 走 parseDaFile/selectDaEntry（mtk_test_helpers.h），
                    # 不补 mtk_da_file.cpp 会在链接期炸（undef）。
                    # D1-T9：bromBringUpDa 走 extractEmiLegacy + PreloaderResult → 补 EMI 提取与
                    # preloader 解析两个模块（后者只依赖 Qt Core、下载器是注入点 —— 用例不联网）。
                    # D1-T9 审查 I1：bromFlashOnSession 落进 mtk_payload.cpp，它还要 芯片表（代际
                    # 判定）与计划层（设备分区表 → 计划）—— 两个都是纯 Qt Core 模块。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_brom.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_da_file.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_emmc.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_payload.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_chip_table.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_preloader_emi.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_preloader_fetch.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/mtk_flash_plan.cpp)
                elseif(_test_stem STREQUAL "test_spd_flash")
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/spd_flash.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/spd_storage.cpp)
                elseif(_test_stem STREQUAL "test_image_worker")
                    # Task 6：ImageWorker 属 UI 层（src/ui，不编入任何静态库），且
                    # 识别入口 doDetect 为私有槽 —— OFP/OPS 尾页探测顺序（A11）只能
                    # 经公开入口 runDetect + detectFinished 信号验证。除被测源外还需：
                    # resource_monitor.cpp（构造里接 CPU 降级信号）、root_patcher
                    # （doPatch 调 patcher::patchFile，链接见下方按名追加）。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/image_worker.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/resource_monitor.cpp)
                elseif(_test_stem STREQUAL "test_pipeline")
                    # F5-2：flashChannelForMode 为 flash_tool.cpp 静态纯函数——整链编译 flash_tool
                    # 及其传递依赖（ADB 采集/EDL/MTK handler/SPD 路由）；插件符号（PluginManager）
                    # 经 phone_plugins 链接，见下方 add_subdirectory(src/plugins) 之后的块。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/flash_tool.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/adb_embedded.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/edl_handler.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_handler.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_brom.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_emmc.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_payload.cpp
                        # Phase D1 Task 9：mtk_handler.cpp 实现 runBromFlash 真流程后引用了
                        # DA 解析/芯片表/EMI/preloader/计划层 —— 不补这些源会在链接期炸（undef）。
                        # Phase D1 Task 10：flash_tool.cpp 的 mtk-brom 通道接线后调用
                        # makeQtPreloaderDownloader（生产下载器，Qt Network；默认关闭、仅
                        # allowNetworkPreloader 时构造）——不补此源同样链接期 undef。
                        # Qt6::Network 在循环后补链给本目标（见 add_subdirectory(src/plugins) 之后）。
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/mtk_flash_plan.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_da_file.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_chip_table.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_preloader_emi.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_preloader_fetch.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_preloader_download_qt.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/spd_flash.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/spd_storage.cpp
                        # Phase B Task 7：edl_handler.cpp 退化为薄封装（持有 LibusbEdlTransport +
                        # EdlSession）后依赖 src/core/edl 整链 —— 不补这些源会在链接期炸（undef）。
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/edl_libusb_transport.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/edl_session.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/firehose.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/flash_plan.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/sahara.cpp
                        # Phase C Task 8：flash_tool.cpp 新增 samsung-odin 通道（odin 计划层 +
                        # 会话 + 真机传输整链）——不补这些源会在链接期炸（undef）。
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/pit.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/samsung_plan.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/odin_protocol.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/odin_session.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/odin_libusb_transport.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/image_engine/tar_image.cpp)
                elseif(_test_stem STREQUAL "test_flash_plan")
                    # Phase B Task 1：计划层（模型 + rawprogram/patch XML 解析）为纯函数模块，
                    # 位于 src/core/edl（已 GLOB 编入主程序，不属任何静态库）——按范例如入本目标。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/flash_plan.cpp)
                elseif(_test_stem STREQUAL "test_edl_sahara")
                    # Phase B Task 4：Sahara 纯协议层（走 IEdlTransport，已 GLOB 编入主程序）
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/sahara.cpp)
                elseif(_test_stem STREQUAL "test_edl_firehose")
                    # Phase B Task 5：Firehose 命令构造/响应解析（纯函数，只依赖 edl_transport.h）
                    # + flash_plan.cpp（PlanEntry/StorageInfo 的定义与校验）。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/firehose.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/flash_plan.cpp)
                elseif(_test_stem STREQUAL "test_edl_session")
                    # Phase B Task 6：会话编排（Sahara → 重枚举 → configure → getstorageinfo →
                    # validatePlan → 逐条目数据面 → reset）整链编入；sparse 展开走
                    # image_engine/sparse_image.cpp 的 sparseWalk（该源已在 image_engine 静态库内，
                    # 这里再显式编入本目标，使"数据面依赖 sparse 解析"在构建图上可见）。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/edl_session.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/firehose.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/flash_plan.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/sahara.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/image_engine/sparse_image.cpp)
                elseif(_test_stem STREQUAL "test_edl_libusb_transport")
                    # Phase B Task 7：真机传输（唯一的 libusb 依赖点）+ 它消费的整链
                    # （会话/协议/计划）编入本目标 —— 使"传输实现只被 IEdlTransport 看见"
                    # 在构建图上可见；libusb 按名补链，见下方 if 块。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/edl_libusb_transport.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/edl_session.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/firehose.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/flash_plan.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/sahara.cpp)
                elseif(_test_stem STREQUAL "test_flash_plan_dialog")
                    # Phase B Task 8：对话框（src/ui，不编入任何静态库）被测源 + 它调用的计划层
                    # （buildPlanFromDir 的整链在 flash_plan.cpp；OFP/OPS 解包走 image_engine）。
                    # 额外链 Qt6::Widgets 与 offscreen 平台，见下方 if 块。
                    # PB-B6：对话框的整包入口把解包搬到 OppoExtractWorker（同样属 src/ui，
                    # 不编入任何静态库）—— 用例直接驱动该 worker 验证"工作线程 + 真实进度 +
                    # 条目边界取消"，故一并编入本目标。
                    # Phase C Task 9：预览骨架抽成 PlanPreviewWidget（src/ui，同样不编入
                    # 任何静态库）—— 对话框改用该控件，本目标随之补编。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/flash_plan_dialog.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/plan_preview_widget.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/oppo_extract_worker.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/edl/flash_plan.cpp)
                elseif(_test_stem STREQUAL "test_pit")
                    # Phase C：PIT 解析纯模块（src/core/odin 已 GLOB 编入主程序，不属任何静态库）
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/pit.cpp)
                elseif(_test_stem STREQUAL "test_samsung_plan")
                    # Phase C：计划层（PIT 匹配）—— 同属 src/core/odin，直接编入本目标
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/pit.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/samsung_plan.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/image_engine/tar_image.cpp)
                elseif(_test_stem STREQUAL "test_odin_protocol")
                    # Phase C：协议帧纯函数层（src/core/odin 已 GLOB 编入主程序，不属任何静态库）；
                    # 它消费 PitEntry（pit.h），故一并编入 pit.cpp。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/pit.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/odin_protocol.cpp)
                elseif(_test_stem STREQUAL "test_odin_session")
                    # Phase C：会话编排（走 IOdinTransport，mock = tests/mock_odin_transport.h）编入
                    # 它的整链 —— 会话消费 samsung_plan（计划层）与 odin_protocol（帧/判定），
                    # 计划层又消费 tar_image（indexTarStream）；四条源在此显式列出，使"会话只经
                    # 传输缝碰设备"在构建图上可见（本目标不含任何 libusb 源）。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/odin_session.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/pit.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/odin_protocol.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/samsung_plan.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/image_engine/tar_image.cpp)
                elseif(_test_stem STREQUAL "test_odin_libusb_transport")
                    # Phase C Task 7：真机传输（odin 侧唯一的 libusb 依赖点）+ 它消费的整链编入本
                    # 目标 —— 使"会话只经 IOdinTransport 碰设备"在构建图上可见；libusb 按名补链，
                    # 见下方 if 块。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/odin_libusb_transport.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/odin_session.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/pit.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/odin_protocol.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/samsung_plan.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/image_engine/tar_image.cpp)
                elseif(_test_stem STREQUAL "test_samsung_plan_dialog")
                    # Phase C Task 9：三星计划对话框（src/ui，不编入任何静态库）+ 通用预览控件
                    # + 计划层/PIT/tar 索引。Qt6::Widgets 与 offscreen 见下方 if 块。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/samsung_plan_dialog.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/plan_preview_widget.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/samsung_plan.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/pit.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/image_engine/tar_image.cpp)
                elseif(_test_stem STREQUAL "test_mtk_da_file")
                    # T9 复审：真样本用例要拿芯片表的 dacode 做查找键（0x0321 → 0x6735），
                    # 故补 mtk_chip_table.cpp（纯 Qt Core，无 libusb）。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_da_file.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_chip_table.cpp)
                elseif(_test_stem STREQUAL "test_mtk_chip_table")
                    # D1 Task 3：芯片表（由 tools/gen_mtk_chip_table.py 从 mtkclient 的
                    # hwconfig 转写；src/core/modes 已 GLOB 编入主程序，不属任何静态库）
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_chip_table.cpp)
                elseif(_test_stem STREQUAL "test_mtk_preloader_emi")
                    # D1 Task 5：preloader EMI 提取（纯函数，只依赖 Qt Core；src/core/modes
                    # 已 GLOB 编入主程序，不属任何静态库）。真样本宏（MTK_SAMPLES_DIR /
                    # MTK_SAMPLES_REQUIRED）已在下方按名命中本 stem，无需重复。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_preloader_emi.cpp)
                elseif(_test_stem STREQUAL "test_mtk_flash_plan")
                    # D1 Task 7：刷写计划层（只依赖 Qt Core —— **不拖 libusb**：设备侧分区表
                    # 由 T9 转换后传入，本模块不 include mtk_emmc/mtk_brom）。真样本宏已在
                    # 下方按名命中本 stem，无需重复。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/mtk_flash_plan.cpp)
                elseif(_test_stem STREQUAL "test_mtk_preloader_fetch")
                    # D1 Task 8：preloader 两路径解析（只依赖 Qt Core、下载器是注入点 ——
                    # 用例全程不联网）。**只编纯模块**：生产下载器 mtk_preloader_download_qt.cpp
                    # （Qt Network）不进本目标，由主程序 GLOB 编译。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_preloader_fetch.cpp)
                endif()
                add_executable(${_test_target} ${_test_src} ${_test_extra_sources})
                if(_test_stem STREQUAL "test_pit" OR _test_stem STREQUAL "test_samsung_plan")
                    # 真样本在 reference/（gitignored）：只把**路径**编进来，用例内 QSKIP 兜底
                    target_compile_definitions(${_test_target} PRIVATE
                        ODIN_SAMPLES_DIR="${PT_ODIN_SAMPLES_DIR}")
                    if(ODIN_SAMPLES_REQUIRED)
                        target_compile_definitions(${_test_target} PRIVATE ODIN_SAMPLES_REQUIRED=1)
                    endif()
                endif()
                # MTK 真样本（reference/mtk-samples/，gitignored）：只把**路径**编进用例。
                # MTK_SAMPLES_REQUIRED=ON 时缺失即 FAIL（QSKIP 不改退出码，ctest 绿灯会骗人）。
                set(MTK_SAMPLES_DIR_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/reference/mtk-samples")
                if(_test_stem STREQUAL "test_mtk_da_file" OR _test_stem STREQUAL "test_mtk_preloader_emi"
                   OR _test_stem STREQUAL "test_mtk_flash_plan")
                    target_compile_definitions(${_test_target} PRIVATE
                        MTK_SAMPLES_DIR="${MTK_SAMPLES_DIR_DEFAULT}")
                    if(MTK_SAMPLES_REQUIRED)
                        target_compile_definitions(${_test_target} PRIVATE MTK_SAMPLES_REQUIRED=1)
                    endif()
                endif()
                target_link_libraries(${_test_target} PRIVATE image_engine Qt6::Test)
                # ImageWorker 测试额外链 root_patcher（image_worker.cpp 的 doPatch 调
                # patcher::patchFile；该库 PUBLIC 传递 image_engine，定义在本循环之前）。
                if(_test_stem STREQUAL "test_image_worker")
                    target_link_libraries(${_test_target} PRIVATE root_patcher)
                endif()
                set_target_properties(${_test_target} PROPERTIES AUTOMOC ON)
                add_test(NAME ${_test_target} COMMAND ${_test_target})
                # Phase B Task 8：对话框测试是仅有的两个带 Widgets 的测试 —— QTEST_MAIN 会造
                # QApplication，无 DISPLAY 的环境必须走 offscreen 平台插件（否则 ctest 当场 abort）。
                # QT_QPA_PLATFORMTHEME 置空：桌面主题插件（如 qt6ct）在无显示环境下加载 GTK 后
                # 直接退出（实测 exit 1，QtTest 还没跑），主题与"勾选框门控"断言无关 —— 置空
                # 让本用例与环境无关（ENVIRONMENT 是**追加**到环境，不能只靠不设置）。
                # Phase C Task 9：三星计划对话框共用同一套 Widgets + offscreen 配置。
                if(_test_stem STREQUAL "test_flash_plan_dialog" OR _test_stem STREQUAL "test_samsung_plan_dialog")
                    target_link_libraries(${_test_target} PRIVATE Qt6::Widgets)
                    set_tests_properties(${_test_target} PROPERTIES
                        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=")
                endif()
                # BROM 协议测试需要 libusb（image_engine 不传递）。
                # 注：原逻辑在 foreach 外按"末次 _test_stem"命中单目标，追加 test_mtk_payload
                # 后 test_mtk_brom 会失去 libusb 链接（undefined symbol）；移入循环按名命中两类目标。
                if(_test_stem STREQUAL "test_mtk_brom" OR _test_stem STREQUAL "test_mtk_payload"
                   OR _test_stem STREQUAL "test_spd_flash" OR _test_stem STREQUAL "test_pipeline"
                   OR _test_stem STREQUAL "test_edl_libusb_transport"
                   OR _test_stem STREQUAL "test_odin_libusb_transport")
                    target_include_directories(${_test_target} PRIVATE ${LIBUSB_INCLUDE_DIRS})
                    target_link_libraries(${_test_target} PRIVATE ${LIBUSB_LIBRARIES})
                endif()
                # 记下 test_pipeline 目标名——phone_plugins 库在本循环之后才定义，
                # 链接放 add_subdirectory(src/plugins) 之后（避免"target not found"）。
                if(_test_stem STREQUAL "test_pipeline")
                    set(_pipeline_test_target ${_test_target})
                endif()
            endforeach()
        endif()
    endif()
endif()

# ---- 插件系统（主项目通用，F2-P）----
add_subdirectory(src/plugins)
target_link_libraries(PhoneToolbox phone_plugins)

# F5-2：test_pipeline 引用 PluginManager（phone_plugins）与 adb_embedded 的
# QNetworkAccessManager（Qt6::Network）——目标在上一节循环已建，此处补链。
if(_pipeline_test_target)
    target_link_libraries(${_pipeline_test_target} PRIVATE phone_plugins Qt6::Network)
endif()

# 可选插件：默认构建（华为刷写），删除目录/关闭选项即完整移除
option(BUILD_HUAWEI_FLASH_PLUGIN "Build Huawei flash plugin" ON)
if(BUILD_HUAWEI_FLASH_PLUGIN AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/plugins/huawei_flash/CMakeLists.txt)
    add_subdirectory(plugins/huawei_flash)
endif()
