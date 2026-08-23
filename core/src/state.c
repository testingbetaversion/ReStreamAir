#include "rs_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// state.json holds admin password hashes, session tokens' hashes, API keys and
// — for script providers — account passwords in the clear, because the script
// has to be handed the real thing. It has no business being readable by anyone
// but the account running the panel, and the platform default would make it
// exactly that. Restricting it at creation (rather than opening it wide and
// tightening afterwards) means it is never briefly readable by anyone else.
//
// POSIX: an explicit 0600 mode, which the umask cannot widen.
// Windows: an explicit owner-only DACL. Windows has no mode bits, and without
// this the file simply inherits whatever the containing directory grants —
// which for a panel unpacked into a shared location is everyone.

#ifdef _WIN32
// A security descriptor whose DACL contains exactly one entry: full access for
// the account this process runs as. Because it is set explicitly at creation,
// the directory's inheritable entries do not apply. Returns false if the SID
// could not be resolved, in which case the caller refuses rather than falling
// back to an unprotected file.
static bool owner_only_sa(SECURITY_ATTRIBUTES *sa, SECURITY_DESCRIPTOR *sd,
                          unsigned char *acl_buf, size_t acl_cap,
                          unsigned char *tok_buf, size_t tok_cap) {
    HANDLE token = NULL;
    DWORD needed = 0;
    TOKEN_USER *user;
    PACL acl = (PACL)acl_buf;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    if (!GetTokenInformation(token, TokenUser, tok_buf, (DWORD)tok_cap, &needed)) {
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);
    user = (TOKEN_USER *)tok_buf;

    if (!InitializeAcl(acl, (DWORD)acl_cap, ACL_REVISION)) return false;
    if (!AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_READ | GENERIC_WRITE | DELETE,
                             user->User.Sid)) return false;
    if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION)) return false;
    if (!SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE)) return false;

    sa->nLength = sizeof(*sa);
    sa->lpSecurityDescriptor = sd;
    sa->bInheritHandle = FALSE;
    return true;
}
#endif

static FILE *open_private(const char *path) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    unsigned char acl_buf[1024];
    unsigned char tok_buf[512];
    HANDLE h;
    int fd;
    FILE *f;

    if (!owner_only_sa(&sa, &sd, acl_buf, sizeof(acl_buf), tok_buf, sizeof(tok_buf)))
        return NULL;

    // CREATE_ALWAYS truncates an existing file but keeps its existing ACL, so
    // the descriptor above only applies to a file we are creating. Removing any
    // previous one first means the restriction is applied every time, including
    // to a state.json that an earlier build wrote without it.
    DeleteFileA(path);
    h = CreateFileA(path, GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(h); return NULL; }
    f = _fdopen(fd, "wb");
    if (!f) _close(fd);   // closes the underlying handle too
    return f;
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
