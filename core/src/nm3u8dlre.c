#include "rs_nm3u8dlre.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "rs_internal.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define F_OK 0
#define access _access
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

static bool file_is_executable(const char *path) {
#ifdef _WIN32
    return access(path, F_OK) == 0;
#else
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR)) {
            return true;
        }
    }
    return false;
#endif
}

static char* check_dir_for_binary(const char *dir, const char *name) {
    if (!dir || !name) return NULL;
    size_t dir_len = strlen(dir);
    if (dir_len == 0) return NULL;
    rs_buf b = RS_BUF_INIT;
    rs_buf_append_str(&b, dir);
#ifdef _WIN32
    if (dir[dir_len - 1] != '\\' && dir[dir_len - 1] != '/') {
        rs_buf_append_char(&b, '\\');
    }
#else
    if (dir[dir_len - 1] != '/') {
        rs_buf_append_char(&b, '/');
    }
#endif
    rs_buf_append_str(&b, name);
    char *path = rs_buf_take(&b);
    if (path && file_is_executable(path)) {
        return path;
    }
    rs_free(path);
    return NULL;
}

char* rs_nm3u8dlre_resolve(void) {
    const char *names[] = {
#ifdef _WIN32
        "N_m3u8DL-RE.exe",
        "N_m3u8DL-RE"
#else
        "N_m3u8DL-RE"
#endif
    };
    size_t names_count = sizeof(names) / sizeof(names[0]);

#ifdef _WIN32
    char sep = ';';
#else
    char sep = ':';
#endif

    const char *env_path = getenv("PATH");
    if (env_path) {
        size_t len = strlen(env_path);
        size_t i = 0;
        while (i < len) {
            size_t start = i;
            while (i < len && env_path[i] != sep) i++;
            const char *dir_str = NULL;
            size_t dir_len = rs_trim(env_path + start, i - start, false, &dir_str);
            if (dir_len > 0) {
                char *dir = rs_trim_dup(dir_str, dir_len, false);
                if (dir) {
                    for (size_t j = 0; j < names_count; j++) {
                        char *found = check_dir_for_binary(dir, names[j]);
                        if (found) {
                            free(dir);
                            return found;
                        }
                    }
                    free(dir);
                }
            }
            if (i < len && env_path[i] == sep) i++;
        }
    }

#ifndef _WIN32
    const char *common_dirs[] = {
        "/opt/homebrew/bin",
        "/usr/local/bin",
        "/usr/bin",
        "/bin",
        "/snap/bin"
    };
    size_t common_dirs_count = sizeof(common_dirs) / sizeof(common_dirs[0]);
    for (size_t i = 0; i < common_dirs_count; i++) {
        for (size_t j = 0; j < names_count; j++) {
            char *found = check_dir_for_binary(common_dirs[i], names[j]);
            if (found) {
                return found;
            }
        }
    }
#endif

    return NULL;
}

char* rs_nm3u8dlre_install_plan(void) {
#if defined(__APPLE__) || defined(__linux__)
    const char *asset = NULL;
#if defined(__APPLE__)
    #if defined(__aarch64__) || defined(_M_ARM64)
        asset = "N_m3u8DL-RE_Beta_macOS_arm64.tar.gz";
    #else
        asset = "N_m3u8DL-RE_Beta_macOS_x64.tar.gz";
    #endif
#elif defined(__linux__)
    #if defined(__aarch64__) || defined(_M_ARM64)
        asset = "N_m3u8DL-RE_Beta_linux-arm64.tar.gz";
    #else
        asset = "N_m3u8DL-RE_Beta_linux-x64.tar.gz";
    #endif
#endif
    
    if (asset) {
        rs_buf b = RS_BUF_INIT;
        rs_buf_append_str(&b, "curl -sLO https://github.com/nilaoda/N_m3u8DL-RE/releases/latest/download/");
        rs_buf_append_str(&b, asset);
        rs_buf_append_str(&b, " && tar -xzf ");
        rs_buf_append_str(&b, asset);
        rs_buf_append_str(&b, " && sudo cp */N_m3u8DL-RE /usr/local/bin/");
        return rs_buf_take(&b);
    }
#endif
    return NULL;
}
