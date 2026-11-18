# Noise-C（MIT，见 COPYING）。按上游 Makefile.am 的 libsodium 后端源列表裁剪，
# 只编译协议层 + libsodium 后端 + 少数曲线/哈希实现。
set(GOLDILOCKS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/crypto/goldilocks/src)

# 32 位 ABI 用 p448/arch_32；64 位用 arch_ref64。
if(MSVC)
    # MSVC 没有 __int128；Curve448 走 32 位实现。
    set(GOLDILOCKS_ARCH_DIR ${GOLDILOCKS_DIR}/p448/arch_32)
elseif(ANDROID AND (ANDROID_ABI STREQUAL "armeabi-v7a" OR ANDROID_ABI STREQUAL "x86"))
    set(GOLDILOCKS_ARCH_DIR ${GOLDILOCKS_DIR}/p448/arch_32)
else()
    set(GOLDILOCKS_ARCH_DIR ${GOLDILOCKS_DIR}/p448/arch_ref64)
endif()

set(NOISE_SOURCES
    src/protocol/cipherstate.c
    src/protocol/dhstate.c
    src/protocol/errors.c
    src/protocol/handshakestate.c
    src/protocol/hashstate.c
    src/protocol/internal.c
    src/protocol/names.c
    src/protocol/patterns.c
    src/protocol/randstate.c
    src/protocol/rand_sodium.c
    src/protocol/signstate.c
    src/protocol/symmetricstate.c
    src/protocol/util.c
    src/backend/sodium/cipher-aesgcm.c
    src/backend/sodium/cipher-chachapoly.c
    src/backend/sodium/dh-curve25519.c
    src/backend/sodium/hash-blake2b.c
    src/backend/sodium/hash-sha256.c
    src/backend/sodium/hash-sha512.c
    src/backend/sodium/sign-ed25519.c
    src/backend/ref/cipher-aesgcm.c
    src/backend/ref/dh-curve448.c
    src/backend/ref/dh-newhope.c
    src/backend/ref/hash-blake2s.c
    src/crypto/aes/rijndael-alg-fst.c
    src/crypto/blake2/blake2s.c
    src/crypto/curve448/curve448.c
    src/crypto/ghash/ghash.c
    ${GOLDILOCKS_ARCH_DIR}/p448.c
    src/crypto/newhope/batcher.c
    src/crypto/newhope/error_correction.c
    src/crypto/newhope/fips202.c
    src/crypto/newhope/newhope.c
    src/crypto/newhope/ntt.c
    src/crypto/newhope/poly.c
    src/crypto/newhope/precomp.c
    src/crypto/newhope/reduce.c)

add_library(noise-c STATIC ${NOISE_SOURCES})

target_include_directories(noise-c PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/protocol
    ${SODIUM_INCLUDE_DIRS}
    ${GOLDILOCKS_DIR}/include
    ${GOLDILOCKS_DIR}/p448
    ${GOLDILOCKS_ARCH_DIR})

target_compile_definitions(noise-c PRIVATE USE_LIBSODIUM=1)
target_compile_options(noise-c PRIVATE -Wall -Wextra)
target_link_libraries(noise-c PUBLIC ${SODIUM_LIBRARIES})
