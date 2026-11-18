add_library(cpace STATIC crypto_cpace.c)
target_include_directories(cpace PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SODIUM_INCLUDE_DIRS})
target_link_libraries(cpace PUBLIC ${SODIUM_LIBRARIES})
target_compile_options(cpace PRIVATE -Wall -Wextra)
