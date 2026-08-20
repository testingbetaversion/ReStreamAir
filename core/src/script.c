#include "rs_script.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <signal.h>  // kill() — timeout kill of the spawned script
extern char **environ;
#endif

static bool needs_encoding(const char *value) {
    for (const char *p = value; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c > 0x7e) return true;
        if (c == '"' || c == '\'' || c == '\\') return true;
    }
    if (strncmp(value, "b64:", 4) == 0) return true;
    return false;
}

char* rs_script_encode_value(const char *value, bool force) {
    if (!value || value[0] == '\0') return rs_strdup(value ? value : "");
    if (!force && !needs_encoding(value)) return rs_strdup(value);
    
    size_t len = strlen(value);
    char *b64 = rs_base64_encode((const uint8_t*)value, len);
    if (!b64) return NULL;
    
    char *res = malloc(4 + strlen(b64) + 1);
    if (res) {
        strcpy(res, "b64:");
        strcat(res, b64);
    }
    rs_free(b64);
    return res;
}

char* rs_script_decode_value(const char *value) {
    if (!value) return NULL;
    if (strncmp(value, "b64:", 4) == 0) {
        const char *encoded = value + 4;
        size_t cap = strlen(encoded);
        uint8_t *out = malloc(cap + 1);
        if (!out) return rs_strdup(value);
        size_t out_len = 0;
        if (rs_base64_decode(encoded, out, cap, &out_len) == 0) {
            char *text = malloc(out_len + 1);
            if (text) {
                memcpy(text, out, out_len);
                text[out_len] = '\0';
                free(out);
                return text;
            }
        }
        free(out);
        return rs_strdup(value);
    }
    
    size_t len = strlen(value);
    if (len >= 4 && len % 4 == 0) {
        bool all_valid = true;
        for (size_t i = 0; i < len; i++) {
            char c = value[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
                all_valid = false;
                break;
            }
        }
        if (all_valid) {
            size_t cap = len;
            uint8_t *out = malloc(cap);
            if (out) {
                size_t out_len = 0;
                if (rs_base64_decode(value, out, cap, &out_len) == 0) {
                    char *text = malloc(out_len + 1);
                    if (text) {
                        memcpy(text, out, out_len);
                        text[out_len] = '\0';
                        
                        bool printable = out_len > 0;
                        for (size_t i = 0; i < out_len; i++) {
                            unsigned char c = text[i];
                            if (c < 0x20 && c != '\n' && c != '\t') {
                                printable = false;
                                break;
                            }
                        }
                        
                        char *reenc = rs_base64_encode(out, out_len);
                        if (reenc && strcmp(reenc, value) == 0 && printable) {
                            free(out);
                            rs_free(reenc);
                            return text;
                        }
                        rs_free(reenc);
                        free(text);
                    }
                }
                free(out);
            }
        }
    }
    
    return rs_strdup(value);
}

rs_strv rs_script_split_params(const char *text) {
    rs_strv out = RS_STRV_INIT;
    if (!text) return out;
    
    size_t len = strlen(text);
    size_t i = 0;
    while (i < len) {
        while (i < len && text[i] == ' ') i++;
        if (i == len) break;
        size_t start = i;
        while (i < len && text[i] != ' ') i++;
        
        size_t tlen = i - start;
        char *token = malloc(tlen + 1);
        if (token) {
            memcpy(token, text + start, tlen);
            token[tlen] = '\0';
            
            char *eq = strchr(token, '=');
            if (eq) {
                *eq = '\0';
                const char *k = token;
                const char *v = eq + 1;
                char *ev = rs_script_encode_value(v, false);
                rs_strv_pushf(&out, "%s=%s", k, ev);
                rs_free(ev);
            } else {
                rs_strv_push(&out, token);
            }
            free(token);
        }
    }
    return out;
}

char* rs_script_arg(const char *key, const char *value, bool force) {
    char *ev = rs_script_encode_value(value, force);
    if (!ev) return NULL;
    char *res = malloc(strlen(key) + 1 + strlen(ev) + 1);
    if (res) {
        strcpy(res, key);
        strcat(res, "=");
        strcat(res, ev);
    }
    rs_free(ev);
    return res;
}

static const char* get_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path) return "";
    return dot + 1;
}

int rs_script_run_sync(const char *script_path, const char **args, int argc, double timeout, char **out_stdout, char **out_stderr) {
    if (out_stdout) *out_stdout = rs_strdup("");
    if (out_stderr) *out_stderr = rs_strdup("");
    
#ifdef _WIN32
    // Windows implementation (placeholder)
    return -1;
#else
    rs_strv spawn_args = RS_STRV_INIT;
    const char *ext = get_ext(script_path);
    if (strcasecmp(ext, "py") == 0) {
        rs_strv_push(&spawn_args, "python3");
        rs_strv_push(&spawn_args, script_path);
    } else if (strcasecmp(ext, "sh") == 0 || strcasecmp(ext, "bash") == 0) {
        rs_strv_push(&spawn_args, "/bin/sh");
        rs_strv_push(&spawn_args, script_path);
    } else {
        rs_strv_push(&spawn_args, script_path);
    }
    for (int i = 0; i < argc; i++) {
        rs_strv_push(&spawn_args, args[i]);
    }
    
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        rs_strv_dispose(&spawn_args);
        return -1;
    }
    
    posix_spawn_file_actions_t action;
    posix_spawn_file_actions_init(&action);
    posix_spawn_file_actions_adddup2(&action, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&action, err_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&action, out_pipe[0]);
    posix_spawn_file_actions_addclose(&action, err_pipe[0]);
    
    pid_t pid;
    const char *exe = spawn_args.items[0];
    if (strcmp(exe, "python3") == 0) exe = "/usr/bin/env"; // resolve python3 off PATH

    if (strcmp(spawn_args.items[0], "python3") == 0) {
        spawn_args.items[0] = "/usr/bin/env";
        rs_strv_dispose(&spawn_args);
        rs_strv_push(&spawn_args, "/usr/bin/env");
        rs_strv_push(&spawn_args, "python3");
        rs_strv_push(&spawn_args, script_path);
        for (int i = 0; i < argc; i++) rs_strv_push(&spawn_args, args[i]);
        exe = spawn_args.items[0];
    }
    
    int status = posix_spawnp(&pid, exe, &action, NULL, spawn_args.items, environ);
    posix_spawn_file_actions_destroy(&action);
    close(out_pipe[1]);
    close(err_pipe[1]);
    rs_strv_dispose(&spawn_args);
    
    if (status != 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        return -1;
    }
    
    rs_buf b_out = RS_BUF_INIT;
    rs_buf b_err = RS_BUF_INIT;
    
    struct pollfd pfds[2];
    pfds[0].fd = out_pipe[0]; pfds[0].events = POLLIN;
    pfds[1].fd = err_pipe[0]; pfds[1].events = POLLIN;
    
    struct timespec start_time, now;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    while (pfds[0].fd >= 0 || pfds[1].fd >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start_time.tv_sec) + (double)(now.tv_nsec - start_time.tv_nsec) / 1e9;
        if (elapsed >= timeout) {
            kill(pid, SIGTERM);
            break;
        }
        
        int wait_ms = (int)((timeout - elapsed) * 1000);
        if (wait_ms < 0) wait_ms = 0;
        
        int r = poll(pfds, 2, wait_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        } else if (r == 0) {
            kill(pid, SIGTERM);
            break;
        }
        
        for (int i = 0; i < 2; i++) {
            if (pfds[i].fd >= 0 && (pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                char buf[4096];
                ssize_t n = read(pfds[i].fd, buf, sizeof(buf));
                if (n > 0) {
                    if (i == 0) rs_buf_append(&b_out, buf, n);
                    else rs_buf_append(&b_err, buf, n);
                } else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                    close(pfds[i].fd);
                    pfds[i].fd = -1;
                }
            }
        }
    }
    
    if (pfds[0].fd >= 0) close(pfds[0].fd);
    if (pfds[1].fd >= 0) close(pfds[1].fd);
    
    int wstatus;
    waitpid(pid, &wstatus, 0);
    
    if (out_stdout) {
        rs_free(*out_stdout);
        *out_stdout = rs_buf_take(&b_out);
        if (!*out_stdout) *out_stdout = rs_strdup("");
    } else {
        rs_buf_dispose(&b_out);
    }
    
    if (out_stderr) {
        rs_free(*out_stderr);
        *out_stderr = rs_buf_take(&b_err);
        if (!*out_stderr) *out_stderr = rs_strdup("");
    } else {
        rs_buf_dispose(&b_err);
    }
    
    if (WIFEXITED(wstatus)) return WEXITSTATUS(wstatus);
    return -1;
#endif
}
