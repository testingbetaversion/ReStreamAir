#ifndef RS_JSON_H
#define RS_JSON_H

// A small JSON DOM. mongoose can query JSON but not hold a mutable tree, and
// the panel's state.json has to be loaded, mutated in a few known places, and
// written back *without losing the fields the C code doesn't model yet* — so a
// real parse/mutate/serialize DOM is the foundation the whole control-plane
// port stands on.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RS_JSON_NULL,
    RS_JSON_BOOL,
    RS_JSON_NUM,
    RS_JSON_STR,
    RS_JSON_ARR,
    RS_JSON_OBJ,
} rs_json_type;

typedef struct rs_json rs_json;

// Parses `len` bytes of JSON. Returns NULL on malformed input or allocation
// failure. The result is released with rs_json_free.
rs_json *rs_json_parse(const char *text, size_t len);

// Serializes to a fresh NUL-terminated string (freed with rs_free). `pretty`
// selects two-space indentation; compact otherwise. NULL on allocation failure.
char *rs_json_serialize(const rs_json *value, bool pretty);

void rs_json_free(rs_json *value);

// Deep copy, so a value from one tree can be placed into another (which then
// owns its copy). NULL on allocation failure.
rs_json *rs_json_clone(const rs_json *value);

// --- readers ---------------------------------------------------------------

rs_json_type rs_json_type_of(const rs_json *value);  // RS_JSON_NULL for a NULL pointer

// Returns the member, or NULL if `obj` isn't an object or the key is absent.
const rs_json *rs_json_obj_get(const rs_json *obj, const char *key);

size_t rs_json_arr_len(const rs_json *arr);
const rs_json *rs_json_arr_at(const rs_json *arr, size_t index);

// Coercing readers: return the value when the type matches, else the fallback.
const char *rs_json_as_str(const rs_json *value, const char *fallback);
double rs_json_as_num(const rs_json *value, double fallback);
bool rs_json_as_bool(const rs_json *value, bool fallback);

// --- builders --------------------------------------------------------------

rs_json *rs_json_new_null(void);
rs_json *rs_json_new_bool(bool b);
rs_json *rs_json_new_num(double n);
rs_json *rs_json_new_int(long long n);   // serializes without a fractional part
rs_json *rs_json_new_str(const char *s);
rs_json *rs_json_new_obj(void);
rs_json *rs_json_new_arr(void);

// Inserts or replaces `key` in an object, taking ownership of `value`. New keys
// keep first-seen order so a round-tripped object stays readable. No-op (and
// frees `value`) if `obj` isn't an object.
void rs_json_obj_set(rs_json *obj, const char *key, rs_json *value);

// Appends to an array, taking ownership. No-op (frees `value`) if not an array.
void rs_json_arr_push(rs_json *arr, rs_json *value);

// Removes a key; returns true if it was present.
bool rs_json_obj_remove(rs_json *obj, const char *key);

// --- convenience readers for request bodies --------------------------------
// Each looks up `key` in an object and coerces, with a fallback when the key is
// absent or the wrong type. Strings coerce a bool/number too, matching how the
// panel's lenient decoders behave.

const char *rs_json_obj_str(const rs_json *obj, const char *key, const char *fallback);
double rs_json_obj_num(const rs_json *obj, const char *key, double fallback);
long long rs_json_obj_int(const rs_json *obj, const char *key, long long fallback);
bool rs_json_obj_bool(const rs_json *obj, const char *key, bool fallback);

// Convenience setters that build and insert the value in one call.
void rs_json_obj_set_str(rs_json *obj, const char *key, const char *value);
void rs_json_obj_set_int(rs_json *obj, const char *key, long long value);
void rs_json_obj_set_bool(rs_json *obj, const char *key, bool value);

#ifdef __cplusplus
}
#endif

#endif  // RS_JSON_H
