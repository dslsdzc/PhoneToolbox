cmake_minimum_required(VERSION 3.16)
project(phone_agent C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(ANDROID)
    if(NOT SODIUM_ROOT)
        message(FATAL_ERROR "SODIUM_ROOT must point to the libsodium Android install prefix")
    endif()
    set(SODIUM_INCLUDE_DIRS ${SODIUM_ROOT}/include)
    set(SODIUM_LIBRARIES ${SODIUM_ROOT}/lib/libsodium.a)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED libsodium)
endif()

find_package(Threads REQUIRED)

add_subdirectory(third_party/cpace)
add_subdirectory(third_party/noise-c)

add_library(agent_core STATIC
    src/frame.c
    src/util.c
    src/audit.c
    src/exec.c
    src/fileops.c
    src/protocol.c
    src/secure/cpace_pair.c
    src/secure/identity.c
    src/secure/noise_channel.c
    third_party/cjson/cJSON.c)

target_include_directories(agent_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cjson
    ${SODIUM_INCLUDE_DIRS})

target_compile_definitions(agent_core PUBLIC _GNU_SOURCE)
target_compile_options(agent_core PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(agent_core PUBLIC ${SODIUM_LIBRARIES} cpace noise-c Threads::Threads)

add_executable(phone_agent
    src/main.c
    src/server.c)
target_link_libraries(phone_agent PRIVATE agent_core)
target_compile_options(phone_agent PRIVATE -Wall -Wextra -Wpedantic)
if(ANDROID)
    target_link_options(phone_agent PRIVATE -pie)
endif()

if(NOT ANDROID)
    enable_testing()

    foreach(test_name IN ITEMS test_frame test_util test_exec test_fileops test_audit
                              test_cpace test_noise)
        add_executable(${test_name} tests/${test_name}.c)
        target_link_libraries(${test_name} PRIVATE agent_core)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    add_executable(test_noise_session tests/test_noise_session.c)
    target_link_libraries(test_noise_session PRIVATE agent_core)
    add_test(NAME test_noise_session
        COMMAND test_noise_session $<TARGET_FILE:phone_agent>)

    add_executable(test_multi_peer tests/test_multi_peer.c)
    target_link_libraries(test_multi_peer PRIVATE agent_core)
    add_test(NAME test_multi_peer
        COMMAND test_multi_peer $<TARGET_FILE:phone_agent>)

    add_executable(test_pair_session tests/test_pair_session.c)
    target_link_libraries(test_pair_session PRIVATE agent_core)
    add_test(NAME test_pair_session
        COMMAND test_pair_session $<TARGET_FILE:phone_agent>)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME test_protocol
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_protocol.py
                    $<TARGET_FILE:phone_agent>)
    endif()
endif()
