#ifndef PHONE_AGENT_LEGACY_COMPAT_H
#define PHONE_AGENT_LEGACY_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(AGENT_LEGACY_STATIC)
// Android 1.x libsodium sysrandom init can hang; use a direct /dev/urandom
// implementation before sodium_init().
void legacy_randombytes_install(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
