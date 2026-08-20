#include "rs_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// state.json holds admin password hashes, session tokens' hashes, API keys and
// — for script providers — account passwords in the clear, because the script
// has to be handed the real thing. It has no business being world-readable, and
// the default umask would make it exactly that. Opening with an explicit 0600
// (rather than fopen + chmod) means it is never briefly readable by anyone else.
static FILE *open_private(const char *path) {
#ifdef _WIN32
    return fopen(path, "wb");
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) close(fd);
    return f;
#endif
}

// Ensures `st->root` names an object array member, creating an empty one if it
// isn't there yet. Returns the (possibly newly created) node.
static rs_json *ensure_array(rs_state *st, const char *key) {
    const rs_json *existing = rs_json_obj_get(st->root, key);
    if (existing && rs_json_type_of(existing) == RS_JSON_ARR) {
        return (rs_json *)existing;
    }
    rs_json *arr = rs_json_new_arr();
    rs_json_obj_set(st->root, key, arr);
    return arr;
}

static rs_json *ensure_object(rs_state *st, const char *key) {
    const rs_json *existing = rs_json_obj_get(st->root, key);
    if (existing && rs_json_type_of(existing) == RS_JSON_OBJ) {
        return (rs_json *)existing;
    }
    rs_json *obj = rs_json_new_obj();
    rs_json_obj_set(st->root, key, obj);
    return obj;
}

int rs_state_load(rs_state *st, const char *path) {
    st->root = NULL;
    st->path = rs_strdup(path);
    if (!st->path) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) {
        // No file yet: start from an empty object, as on
        // a fresh install. Not an error.
        st->root = rs_json_new_obj();
        return st->root ? 0 : -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    rewind(f);

    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) { fclose(f); return -1; }
    size_t read = fread(buffer, 1, (size_t)size, f);
    fclose(f);
    buffer[read] = '\0';

    // An empty file is treated like a missing one rather than a parse error.
    rs_json *root = read == 0 ? rs_json_new_obj() : rs_json_parse(buffer, read);
    free(buffer);
    if (!root) return -1;
    if (rs_json_type_of(root) != RS_JSON_OBJ) {  // state.json must be an object
        rs_json_free(root);
        return -1;
    }
    st->root = root;
    return 0;
}

int rs_state_save(const rs_state *st) {
    if (!st || !st->root || !st->path) return -1;
    char *text = rs_json_serialize(st->root, true);
    if (!text) return -1;

    // Write to a temp file next to the target, then rename — so a crash mid-write
    // can never leave a truncated state.json.
    size_t path_len = strlen(st->path);
    char *tmp = (char *)malloc(path_len + 5);
    if (!tmp) { rs_free(text); return -1; }
    memcpy(tmp, st->path, path_len);
    memcpy(tmp + path_len, ".tmp", 5);

    FILE *f = open_private(tmp);
    if (!f) { free(tmp); rs_free(text); return -1; }
    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, f);
    int flush_ok = (fflush(f) == 0);
    fclose(f);
    rs_free(text);
    if (written != len || !flush_ok) { remove(tmp); free(tmp); return -1; }

    if (rename(tmp, st->path) != 0) { remove(tmp); free(tmp); return -1; }
#ifndef _WIN32
    // rename() keeps the temp file's mode, but an existing state.json written
    // by an older build (or restored from a backup) keeps its own — tighten it
    // either way, every save.
    chmod(st->path, S_IRUSR | S_IWUSR);
#endif
    free(tmp);
    return 0;
}

void rs_state_dispose(rs_state *st) {
    if (!st) return;
    rs_json_free(st->root);
    free(st->path);
    st->root = NULL;
    st->path = NULL;
}

rs_json *rs_state_admin_users(rs_state *st) {
    return ensure_array(st, "adminUsers");
}

rs_json *rs_state_settings(rs_state *st) {
    return ensure_object(st, "settings");
}

const rs_json *rs_state_providers(const rs_state *st) {
    return rs_json_obj_get(st->root, "providers");
}

const rs_json *rs_state_api_keys(const rs_state *st) {
    return rs_json_obj_get(st->root, "apiKeys");
}
