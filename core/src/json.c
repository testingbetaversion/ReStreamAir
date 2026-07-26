#include "rs_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs_internal.h"

// Objects keep their members in an insertion-ordered array rather than a hash
// map: state.json objects are small, and preserving key order keeps a
// round-tripped file readable and its diffs minimal.
typedef struct {
    char *key;
    rs_json *value;
} rs_json_member;

struct rs_json {
    rs_json_type type;
    union {
        bool boolean;
        struct {
            double number;
            bool is_integer;
        } num;
        char *string;
        struct {
            rs_json **items;
            size_t len;
            size_t cap;
        } arr;
        struct {
            rs_json_member *members;
            size_t len;
            size_t cap;
        } obj;
    } as;
};

// --- construction ----------------------------------------------------------

static rs_json *alloc_node(rs_json_type type) {
    rs_json *node = (rs_json *)calloc(1, sizeof(rs_json));
    if (node) node->type = type;
    return node;
}

rs_json *rs_json_new_null(void) { return alloc_node(RS_JSON_NULL); }

rs_json *rs_json_new_bool(bool b) {
    rs_json *node = alloc_node(RS_JSON_BOOL);
    if (node) node->as.boolean = b;
    return node;
}

rs_json *rs_json_new_num(double n) {
    rs_json *node = alloc_node(RS_JSON_NUM);
    if (node) {
        node->as.num.number = n;
        node->as.num.is_integer = false;
    }
    return node;
}

rs_json *rs_json_new_int(long long n) {
    rs_json *node = alloc_node(RS_JSON_NUM);
    if (node) {
        node->as.num.number = (double)n;
        node->as.num.is_integer = true;
    }
    return node;
}

rs_json *rs_json_new_str(const char *s) {
    rs_json *node = alloc_node(RS_JSON_STR);
    if (!node) return NULL;
    node->as.string = rs_strdup(s ? s : "");
    if (!node->as.string) { free(node); return NULL; }
    return node;
}

rs_json *rs_json_new_obj(void) { return alloc_node(RS_JSON_OBJ); }
rs_json *rs_json_new_arr(void) { return alloc_node(RS_JSON_ARR); }

void rs_json_free(rs_json *value) {
    if (!value) return;
    switch (value->type) {
        case RS_JSON_STR:
            free(value->as.string);
            break;
        case RS_JSON_ARR:
            for (size_t i = 0; i < value->as.arr.len; i++) rs_json_free(value->as.arr.items[i]);
            free(value->as.arr.items);
            break;
        case RS_JSON_OBJ:
            for (size_t i = 0; i < value->as.obj.len; i++) {
                free(value->as.obj.members[i].key);
                rs_json_free(value->as.obj.members[i].value);
            }
            free(value->as.obj.members);
            break;
        default:
            break;
    }
    free(value);
}

rs_json *rs_json_clone(const rs_json *value) {
    if (!value) return NULL;
    switch (value->type) {
        case RS_JSON_NULL: return rs_json_new_null();
        case RS_JSON_BOOL: return rs_json_new_bool(value->as.boolean);
        case RS_JSON_NUM: {
            rs_json *node = alloc_node(RS_JSON_NUM);
            if (node) node->as.num = value->as.num;
            return node;
        }
        case RS_JSON_STR: return rs_json_new_str(value->as.string);
        case RS_JSON_ARR: {
            rs_json *arr = rs_json_new_arr();
            if (!arr) return NULL;
            for (size_t i = 0; i < value->as.arr.len; i++) {
                rs_json *item = rs_json_clone(value->as.arr.items[i]);
                if (!item) { rs_json_free(arr); return NULL; }
                rs_json_arr_push(arr, item);
            }
            return arr;
        }
        case RS_JSON_OBJ: {
            rs_json *obj = rs_json_new_obj();
            if (!obj) return NULL;
            for (size_t i = 0; i < value->as.obj.len; i++) {
                rs_json *copy = rs_json_clone(value->as.obj.members[i].value);
                if (!copy) { rs_json_free(obj); return NULL; }
                rs_json_obj_set(obj, value->as.obj.members[i].key, copy);
            }
            return obj;
        }
    }
    return NULL;
}

// --- mutation --------------------------------------------------------------

void rs_json_arr_push(rs_json *arr, rs_json *value) {
    if (!arr || arr->type != RS_JSON_ARR) { rs_json_free(value); return; }
    if (arr->as.arr.len + 1 > arr->as.arr.cap) {
        size_t cap = arr->as.arr.cap ? arr->as.arr.cap * 2 : 8;
        rs_json **items = (rs_json **)realloc(arr->as.arr.items, cap * sizeof(rs_json *));
        if (!items) { rs_json_free(value); return; }
        arr->as.arr.items = items;
        arr->as.arr.cap = cap;
    }
    arr->as.arr.items[arr->as.arr.len++] = value;
}

void rs_json_obj_set(rs_json *obj, const char *key, rs_json *value) {
    if (!obj || obj->type != RS_JSON_OBJ || !key) { rs_json_free(value); return; }
    for (size_t i = 0; i < obj->as.obj.len; i++) {
        if (strcmp(obj->as.obj.members[i].key, key) == 0) {
            rs_json_free(obj->as.obj.members[i].value);
            obj->as.obj.members[i].value = value;
            return;
        }
    }
    if (obj->as.obj.len + 1 > obj->as.obj.cap) {
        size_t cap = obj->as.obj.cap ? obj->as.obj.cap * 2 : 8;
        rs_json_member *members = (rs_json_member *)realloc(obj->as.obj.members, cap * sizeof(rs_json_member));
        if (!members) { rs_json_free(value); return; }
        obj->as.obj.members = members;
        obj->as.obj.cap = cap;
    }
    char *copy = rs_strdup(key);
    if (!copy) { rs_json_free(value); return; }
    obj->as.obj.members[obj->as.obj.len].key = copy;
    obj->as.obj.members[obj->as.obj.len].value = value;
    obj->as.obj.len++;
}

bool rs_json_obj_remove(rs_json *obj, const char *key) {
    if (!obj || obj->type != RS_JSON_OBJ || !key) return false;
    for (size_t i = 0; i < obj->as.obj.len; i++) {
        if (strcmp(obj->as.obj.members[i].key, key) == 0) {
            free(obj->as.obj.members[i].key);
            rs_json_free(obj->as.obj.members[i].value);
            memmove(&obj->as.obj.members[i], &obj->as.obj.members[i + 1],
                    (obj->as.obj.len - i - 1) * sizeof(rs_json_member));
            obj->as.obj.len--;
            return true;
        }
    }
    return false;
}

// --- readers ---------------------------------------------------------------

rs_json_type rs_json_type_of(const rs_json *value) {
    return value ? value->type : RS_JSON_NULL;
}

const rs_json *rs_json_obj_get(const rs_json *obj, const char *key) {
    if (!obj || obj->type != RS_JSON_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->as.obj.len; i++) {
        if (strcmp(obj->as.obj.members[i].key, key) == 0) return obj->as.obj.members[i].value;
    }
    return NULL;
}

size_t rs_json_arr_len(const rs_json *arr) {
    return (arr && arr->type == RS_JSON_ARR) ? arr->as.arr.len : 0;
}

const rs_json *rs_json_arr_at(const rs_json *arr, size_t index) {
    if (!arr || arr->type != RS_JSON_ARR || index >= arr->as.arr.len) return NULL;
    return arr->as.arr.items[index];
}

const char *rs_json_as_str(const rs_json *value, const char *fallback) {
    return (value && value->type == RS_JSON_STR) ? value->as.string : fallback;
}

double rs_json_as_num(const rs_json *value, double fallback) {
    return (value && value->type == RS_JSON_NUM) ? value->as.num.number : fallback;
}

bool rs_json_as_bool(const rs_json *value, bool fallback) {
    return (value && value->type == RS_JSON_BOOL) ? value->as.boolean : fallback;
}

// --- parser ----------------------------------------------------------------

#define RS_JSON_MAX_DEPTH 200

typedef struct {
    const char *p;
    const char *end;
    int depth;
} parser;

static void skip_ws(parser *ps) {
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ps->p++;
        else break;
    }
}

static rs_json *parse_value(parser *ps);

static int parse_hex4(const char *s, unsigned *out) {
    unsigned value = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        value <<= 4;
        if (c >= '0' && c <= '9') value |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') value |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = value;
    return 0;
}

static void encode_utf8(unsigned cp, rs_buf *out) {
    if (cp < 0x80) {
        rs_buf_append_char(out, (char)cp);
    } else if (cp < 0x800) {
        rs_buf_append_char(out, (char)(0xc0 | (cp >> 6)));
        rs_buf_append_char(out, (char)(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        rs_buf_append_char(out, (char)(0xe0 | (cp >> 12)));
        rs_buf_append_char(out, (char)(0x80 | ((cp >> 6) & 0x3f)));
        rs_buf_append_char(out, (char)(0x80 | (cp & 0x3f)));
    } else {
        rs_buf_append_char(out, (char)(0xf0 | (cp >> 18)));
        rs_buf_append_char(out, (char)(0x80 | ((cp >> 12) & 0x3f)));
        rs_buf_append_char(out, (char)(0x80 | ((cp >> 6) & 0x3f)));
        rs_buf_append_char(out, (char)(0x80 | (cp & 0x3f)));
    }
}

// Parses a JSON string (the opening quote already consumed) into a fresh
// NUL-terminated buffer. Returns NULL on malformed input.
static char *parse_string_raw(parser *ps) {
    rs_buf out = RS_BUF_INIT;
    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"') {
            char *result = rs_buf_take(&out);
            return result ? result : NULL;
        }
        if (c == '\\') {
            if (ps->p >= ps->end) break;
            char esc = *ps->p++;
            switch (esc) {
                case '"': rs_buf_append_char(&out, '"'); break;
                case '\\': rs_buf_append_char(&out, '\\'); break;
                case '/': rs_buf_append_char(&out, '/'); break;
                case 'b': rs_buf_append_char(&out, '\b'); break;
                case 'f': rs_buf_append_char(&out, '\f'); break;
                case 'n': rs_buf_append_char(&out, '\n'); break;
                case 'r': rs_buf_append_char(&out, '\r'); break;
                case 't': rs_buf_append_char(&out, '\t'); break;
                case 'u': {
                    unsigned cp;
                    if (ps->end - ps->p < 4 || parse_hex4(ps->p, &cp) != 0) { rs_buf_dispose(&out); return NULL; }
                    ps->p += 4;
                    // Combine a surrogate pair into one code point.
                    if (cp >= 0xd800 && cp <= 0xdbff && ps->end - ps->p >= 6 &&
                        ps->p[0] == '\\' && ps->p[1] == 'u') {
                        unsigned lo;
                        if (parse_hex4(ps->p + 2, &lo) == 0 && lo >= 0xdc00 && lo <= 0xdfff) {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                            ps->p += 6;
                        }
                    }
                    encode_utf8(cp, &out);
                    break;
                }
                default:
                    rs_buf_dispose(&out);
                    return NULL;
            }
        } else {
            rs_buf_append_char(&out, c);
        }
    }
    rs_buf_dispose(&out);
    return NULL;
}

static rs_json *parse_string(parser *ps) {
    char *raw = parse_string_raw(ps);
    if (!raw) return NULL;
    rs_json *node = alloc_node(RS_JSON_STR);
    if (!node) { free(raw); return NULL; }
    node->as.string = raw;
    return node;
}

static rs_json *parse_number(parser *ps) {
    const char *start = ps->p;
    bool is_integer = true;
    if (ps->p < ps->end && (*ps->p == '-' || *ps->p == '+')) ps->p++;
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c >= '0' && c <= '9') {
            ps->p++;
        } else if (c == '.' || c == 'e' || c == 'E') {
            is_integer = false;
            ps->p++;
        } else if (c == '+' || c == '-') {
            ps->p++;  // exponent sign
        } else {
            break;
        }
    }
    if (ps->p == start) return NULL;
    char buf[64];
    size_t n = (size_t)(ps->p - start);
    if (n >= sizeof(buf)) return NULL;
    memcpy(buf, start, n);
    buf[n] = '\0';
    double value = strtod(buf, NULL);
    rs_json *node = alloc_node(RS_JSON_NUM);
    if (!node) return NULL;
    node->as.num.number = value;
    // Integer syntax (no '.', 'e' or 'E') already means no fractional part, so
    // the value stays integer on output as long as it's within the range a
    // double represents exactly — no libm floor()/fabs() needed.
    node->as.num.is_integer = is_integer && value >= -9.0e15 && value <= 9.0e15;
    return node;
}

static bool match_literal(parser *ps, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0) return false;
    ps->p += n;
    return true;
}

static rs_json *parse_array(parser *ps) {
    ps->p++;  // consume '['
    rs_json *arr = rs_json_new_arr();
    if (!arr) return NULL;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return arr; }
    for (;;) {
        rs_json *item = parse_value(ps);
        if (!item) { rs_json_free(arr); return NULL; }
        rs_json_arr_push(arr, item);
        skip_ws(ps);
        if (ps->p >= ps->end) { rs_json_free(arr); return NULL; }
        char c = *ps->p++;
        if (c == ',') { skip_ws(ps); continue; }
        if (c == ']') return arr;
        rs_json_free(arr);
        return NULL;
    }
}

static rs_json *parse_object(parser *ps) {
    ps->p++;  // consume '{'
    rs_json *obj = rs_json_new_obj();
    if (!obj) return NULL;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return obj; }
    for (;;) {
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') { rs_json_free(obj); return NULL; }
        ps->p++;
        char *key = parse_string_raw(ps);
        if (!key) { rs_json_free(obj); return NULL; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { free(key); rs_json_free(obj); return NULL; }
        ps->p++;
        rs_json *value = parse_value(ps);
        if (!value) { free(key); rs_json_free(obj); return NULL; }
        rs_json_obj_set(obj, key, value);
        free(key);
        skip_ws(ps);
        if (ps->p >= ps->end) { rs_json_free(obj); return NULL; }
        char c = *ps->p++;
        if (c == ',') continue;
        if (c == '}') return obj;
        rs_json_free(obj);
        return NULL;
    }
}

static rs_json *parse_value(parser *ps) {
    if (++ps->depth > RS_JSON_MAX_DEPTH) { ps->depth--; return NULL; }
    skip_ws(ps);
    rs_json *result = NULL;
    if (ps->p < ps->end) {
        char c = *ps->p;
        if (c == '{') result = parse_object(ps);
        else if (c == '[') result = parse_array(ps);
        else if (c == '"') { ps->p++; result = parse_string(ps); }
        else if (c == '-' || (c >= '0' && c <= '9')) result = parse_number(ps);
        else if (match_literal(ps, "true")) result = rs_json_new_bool(true);
        else if (match_literal(ps, "false")) result = rs_json_new_bool(false);
        else if (match_literal(ps, "null")) result = rs_json_new_null();
    }
    ps->depth--;
    return result;
}

rs_json *rs_json_parse(const char *text, size_t len) {
    if (!text) return NULL;
    parser ps = {text, text + len, 0};
    rs_json *value = parse_value(&ps);
    if (!value) return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) {  // trailing garbage
        rs_json_free(value);
        return NULL;
    }
    return value;
}

// --- serializer ------------------------------------------------------------

static void append_escaped(rs_buf *out, const char *s) {
    static const char hex[] = "0123456789abcdef";
    rs_buf_append_char(out, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
            case '"': rs_buf_append_str(out, "\\\""); break;
            case '\\': rs_buf_append_str(out, "\\\\"); break;
            case '\b': rs_buf_append_str(out, "\\b"); break;
            case '\f': rs_buf_append_str(out, "\\f"); break;
            case '\n': rs_buf_append_str(out, "\\n"); break;
            case '\r': rs_buf_append_str(out, "\\r"); break;
            case '\t': rs_buf_append_str(out, "\\t"); break;
            default:
                if (c < 0x20) {
                    char esc[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0xf], '\0'};
                    rs_buf_append_str(out, esc);
                } else {
                    rs_buf_append_char(out, (char)c);  // UTF-8 passes through
                }
        }
    }
    rs_buf_append_char(out, '"');
}

static void append_number(rs_buf *out, const rs_json *value) {
    char buf[32];
    if (value->as.num.is_integer) {
        snprintf(buf, sizeof(buf), "%lld", (long long)value->as.num.number);
    } else {
        // 17 significant digits round-trips a double exactly.
        snprintf(buf, sizeof(buf), "%.17g", value->as.num.number);
    }
    rs_buf_append_str(out, buf);
}

static void append_indent(rs_buf *out, int depth) {
    for (int i = 0; i < depth; i++) rs_buf_append_str(out, "  ");
}

static void serialize_value(rs_buf *out, const rs_json *value, bool pretty, int depth) {
    switch (value->type) {
        case RS_JSON_NULL: rs_buf_append_str(out, "null"); break;
        case RS_JSON_BOOL: rs_buf_append_str(out, value->as.boolean ? "true" : "false"); break;
        case RS_JSON_NUM: append_number(out, value); break;
        case RS_JSON_STR: append_escaped(out, value->as.string); break;
        case RS_JSON_ARR:
            if (value->as.arr.len == 0) { rs_buf_append_str(out, "[]"); break; }
            rs_buf_append_char(out, '[');
            for (size_t i = 0; i < value->as.arr.len; i++) {
                if (i) rs_buf_append_char(out, ',');
                if (pretty) { rs_buf_append_char(out, '\n'); append_indent(out, depth + 1); }
                serialize_value(out, value->as.arr.items[i], pretty, depth + 1);
            }
            if (pretty) { rs_buf_append_char(out, '\n'); append_indent(out, depth); }
            rs_buf_append_char(out, ']');
            break;
        case RS_JSON_OBJ:
            if (value->as.obj.len == 0) { rs_buf_append_str(out, "{}"); break; }
            rs_buf_append_char(out, '{');
            for (size_t i = 0; i < value->as.obj.len; i++) {
                if (i) rs_buf_append_char(out, ',');
                if (pretty) { rs_buf_append_char(out, '\n'); append_indent(out, depth + 1); }
                append_escaped(out, value->as.obj.members[i].key);
                rs_buf_append_str(out, pretty ? ": " : ":");
                serialize_value(out, value->as.obj.members[i].value, pretty, depth + 1);
            }
            if (pretty) { rs_buf_append_char(out, '\n'); append_indent(out, depth); }
            rs_buf_append_char(out, '}');
            break;
    }
}

char *rs_json_serialize(const rs_json *value, bool pretty) {
    if (!value) return NULL;
    rs_buf out = RS_BUF_INIT;
    serialize_value(&out, value, pretty, 0);
    return rs_buf_take(&out);
}

// --- convenience accessors -------------------------------------------------

const char *rs_json_obj_str(const rs_json *obj, const char *key, const char *fallback) {
    return rs_json_as_str(rs_json_obj_get(obj, key), fallback);
}

double rs_json_obj_num(const rs_json *obj, const char *key, double fallback) {
    return rs_json_as_num(rs_json_obj_get(obj, key), fallback);
}

long long rs_json_obj_int(const rs_json *obj, const char *key, long long fallback) {
    const rs_json *v = rs_json_obj_get(obj, key);
    if (v && v->type == RS_JSON_NUM) return (long long)v->as.num.number;
    return fallback;
}

bool rs_json_obj_bool(const rs_json *obj, const char *key, bool fallback) {
    return rs_json_as_bool(rs_json_obj_get(obj, key), fallback);
}

void rs_json_obj_set_str(rs_json *obj, const char *key, const char *value) {
    rs_json_obj_set(obj, key, rs_json_new_str(value ? value : ""));
}

void rs_json_obj_set_int(rs_json *obj, const char *key, long long value) {
    rs_json_obj_set(obj, key, rs_json_new_int(value));
}

void rs_json_obj_set_bool(rs_json *obj, const char *key, bool value) {
    rs_json_obj_set(obj, key, rs_json_new_bool(value));
}
