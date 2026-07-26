#include "rs_common.h"

// Cryptographically secure random bytes, for password salts and session tokens.

#if defined(_WIN32)

#include <windows.h>
#include <bcrypt.h>

int rs_random_bytes(uint8_t *out, size_t len) {
    if (len == 0) return 0;
    NTSTATUS status = BCryptGenRandom(NULL, out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0 ? 0 : -1;
}

#else

#include <stddef.h>
#include <stdio.h>

#if defined(__APPLE__) || defined(__GLIBC__)
#include <sys/random.h>  // getentropy
#define RS_HAVE_GETENTROPY 1
#endif

int rs_random_bytes(uint8_t *out, size_t len) {
    if (len == 0) return 0;

#ifdef RS_HAVE_GETENTROPY
    // getentropy caps at 256 bytes per call.
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset < 256 ? len - offset : 256;
        if (getentropy(out + offset, chunk) != 0) break;
        offset += chunk;
    }
    if (offset == len) return 0;
#endif

    // Fallback for platforms without getentropy, or if it failed.
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t read = fread(out, 1, len, f);
    fclose(f);
    return read == len ? 0 : -1;
}

#endif
