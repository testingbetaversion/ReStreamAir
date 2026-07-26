#ifndef RS_INTERNAL_H
#define RS_INTERNAL_H

// Internal helpers shared between core/src translation units. Not installed and
// not part of the public API — public headers live in core/include.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Growable byte buffer with a sticky error flag: once an allocation fails every
// subsequent append is a no-op and rs_buf_take returns NULL, so call sites can
// append freely and check once at the end.
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool err;
} rs_buf;

#define RS_BUF_INIT {NULL, 0, 0, false}

void rs_buf_append(rs_buf *b, const char *bytes, size_t len);
void rs_buf_append_str(rs_buf *b, const char *s);
void rs_buf_append_char(rs_buf *b, char c);
void rs_buf_appendf(rs_buf *b, const char *fmt, ...);

// Hands the NUL-terminated contents to the caller (freed with rs_free) and
// resets the buffer. Returns NULL if any append failed.
char *rs_buf_take(rs_buf *b);

// Frees the buffer's storage without producing a result.
void rs_buf_dispose(rs_buf *b);

// Growable array of owned strings, with the same sticky-error behaviour.
typedef struct {
    char **items;
    size_t len;
    size_t cap;
    bool err;
} rs_strv;

#define RS_STRV_INIT {NULL, 0, 0, false}

void rs_strv_push(rs_strv *v, const char *s);        // copies s
void rs_strv_push_owned(rs_strv *v, char *s);        // takes ownership of s
void rs_strv_pushf(rs_strv *v, const char *fmt, ...);
void rs_strv_dispose(rs_strv *v);

// True for the characters Swift's CharacterSet.whitespaces matches in ASCII.
bool rs_is_space(char c);
// Adds the line terminators, matching .whitespacesAndNewlines.
bool rs_is_space_nl(char c);

// Narrows [s, s+len) to the trimmed range. `nl` selects which set to trim.
// Writes the new start into *out and returns the new length.
size_t rs_trim(const char *s, size_t len, bool nl, const char **out);

// Copies the trimmed range into a fresh NUL-terminated string.
char *rs_trim_dup(const char *s, size_t len, bool nl);

#ifdef __cplusplus
}
#endif

#endif  // RS_INTERNAL_H
