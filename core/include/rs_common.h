#ifndef RS_COMMON_H
#define RS_COMMON_H

// Conventions shared by every rs_* module in the core:
//
//   * Functions that can fail return int: 0 on success, negative on failure.
//     Functions that cannot fail return void or the value directly.
//   * Every heap allocation that crosses the library boundary is released with
//     rs_free (or rs_free_strv for string arrays). Callers must not use their
//     own free() — on Windows the caller may link a different CRT heap.
//   * Contexts are caller-allocated plain structs, so callers can put them on
//     the stack and no hidden allocation happens on hot paths.
//   * Strings are NUL-terminated UTF-8. Byte buffers are (pointer, size_t).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// strcasecmp / strncasecmp are POSIX and live in <strings.h>, which the MSVC
// CRT does not have — there they are _stricmp / _strnicmp in <string.h>.
// Every module that compares case-insensitively already includes this header,
// so the spelling is reconciled once here rather than in each of them.
#ifdef _WIN32
#include <string.h>
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Releases memory returned by any rs_* function. NULL is a no-op.
void rs_free(void *p);

// Releases an array of `n` strings and the array itself. NULL is a no-op.
void rs_free_strv(char **v, size_t n);

// strdup, which is not in C11 and is spelled _strdup by MSVC.
// Returns NULL if s is NULL or allocation fails.
char *rs_strdup(const char *s);

// Fills `out` with `len` cryptographically secure random bytes (getentropy /
// /dev/urandom on POSIX, BCryptGenRandom on Windows). Returns 0 on success,
// negative if no entropy source was available. Used for salts and session
// tokens, so a failure must be treated as fatal by the caller.
int rs_random_bytes(uint8_t *out, size_t len);

// Base64 encode/decode. rs_base64_encode returns a fresh NUL-terminated string
// (rs_free). rs_base64_decode writes into `out` (which must hold at least
// 3*len/4 bytes) and reports the decoded length via *out_len; returns 0 on
// success, -1 on invalid input.
char *rs_base64_encode(const uint8_t *data, size_t len);
int rs_base64_decode(const char *text, uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif  // RS_COMMON_H
