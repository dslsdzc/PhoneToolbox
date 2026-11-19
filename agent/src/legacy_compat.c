/* Android 1.x/4.x bionic has no __isinf/__isnan/__signbit symbols. */
#include "legacy_compat.h"

#if defined(__ANDROID__) && __ANDROID_API__ < 21
int __isinf(double value)
{
    if (value != value)
        return 0;
    const double inf = 1.0 / 0.0;
    if (value == inf)
        return 1;
    if (value == -inf)
        return -1;
    return 0;
}

int __isnan(double value)
{
    return value != value;
}

int __signbit(double value)
{
    if (value < 0.0)
        return 1;
    if (value == 0.0)
        return (1.0 / value) < 0.0;
    return 0;
}
#endif

#if defined(AGENT_LEGACY_STATIC)
#include <errno.h>
#include <fcntl.h>
#include <sodium/randombytes.h>
#include <unistd.h>

static int legacy_random_fd = -1;

static const char *legacy_random_name(void)
{
    return "legacy-urandom";
}

/* Never hand out predictable bytes: without /dev/urandom identity keys would
 * silently degrade to zeros. */
static void legacy_random_fatal(void)
{
    static const char message[] = "phone_agent: cannot read /dev/urandom\n";
    (void) write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(1);
}

static uint32_t legacy_random_u32(void)
{
    uint32_t value = 0;
    size_t done = 0;
    while (done < sizeof(value)) {
        const ssize_t got = legacy_random_fd >= 0
                                ? read(legacy_random_fd,
                                       (unsigned char *) &value + done,
                                       sizeof(value) - done)
                                : -1;
        if (got > 0) {
            done += (size_t) got;
        } else if (got < 0 && errno == EINTR) {
            continue;
        } else {
            legacy_random_fatal();
        }
    }
    return value;
}

static void legacy_random_buf(void *buffer, size_t size)
{
    unsigned char *out = (unsigned char *) buffer;
    while (size > 0) {
        const ssize_t got = legacy_random_fd >= 0
                                ? read(legacy_random_fd, out, size)
                                : -1;
        if (got > 0) {
            out += got;
            size -= (size_t) got;
        } else if (got < 0 && errno == EINTR) {
            continue;
        } else {
            legacy_random_fatal();
        }
    }
}

static void legacy_random_stir(void)
{
    if (legacy_random_fd >= 0)
        return;
    do {
        legacy_random_fd = open("/dev/urandom", O_RDONLY);
    } while (legacy_random_fd < 0 && errno == EINTR);
    if (legacy_random_fd < 0)
        legacy_random_fatal();
}

static int legacy_random_close(void)
{
    if (legacy_random_fd >= 0) {
        close(legacy_random_fd);
        legacy_random_fd = -1;
    }
    return 0;
}

static const randombytes_implementation legacy_random_impl = {
    .implementation_name = legacy_random_name,
    .random = legacy_random_u32,
    .stir = legacy_random_stir,
    .uniform = NULL,
    .buf = legacy_random_buf,
    .close = legacy_random_close
};

void legacy_randombytes_install(void)
{
    randombytes_set_implementation(
        (randombytes_implementation *) &legacy_random_impl);
}
#endif
