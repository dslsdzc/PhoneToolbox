#include "util.h"

#include <sodium.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            return 1;                                                            \
        }                                                                        \
    } while (0)

int main(void)
{
    CHECK(sodium_init() >= 0);

    char digest[65];
    sha256_hex("abc", 3, digest);
    CHECK(strcmp(digest,
                 "ba7816bf8f01cfea414140de5dae2223"
                 "b00361a396177a9cb410ff61f20015ad") == 0);

    CHECK(path_is_safe("/data/local/tmp/x.bin"));
    CHECK(path_is_safe("/sdcard/Download/a b.bin"));
    CHECK(!path_is_safe("relative"));
    CHECK(!path_is_safe("/"));
    CHECK(!path_is_safe("/data/"));
    CHECK(!path_is_safe("/data/../etc/passwd"));
    CHECK(!path_is_safe("/data/./x"));
    CHECK(!path_is_safe("/data//x"));
    CHECK(!path_is_safe("/data/x\n"));
    CHECK(!path_is_safe(""));

    char *random_a = NULL;
    char *random_b = NULL;
    CHECK(random_hex(16, &random_a) == 0);
    CHECK(random_hex(16, &random_b) == 0);
    CHECK(strlen(random_a) == 32);
    CHECK(strlen(random_b) == 32);
    CHECK(strcmp(random_a, random_b) != 0);
    free(random_a);
    free(random_b);

    printf("test_util: OK\n");
    return 0;
}
