// The portable half of running child processes. See rs_proc.h for the contract.
//
// The two platform sections below are deliberately complete rather than
// "POSIX plus a few #ifdefs": fd inheritance and HANDLE inheritance are
// different enough that interleaving them produces code nobody can audit. What
// they share — draining two pipes without deadlocking, the timeout ladder,
// picking an interpreter — lives once at the bottom.

#include "rs_proc.h"
#include "rs_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===========================================================================
#ifndef _WIN32
// ===========================================================================

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

int rs_pipe_open(rs_pipe *p) {
    int fds[2];
    if (!p) return -1;
    if (pipe(fds) != 0) return -1;
    // Both ends close-on-exec, so a child only ever receives what rs_proc_spawn
    // explicitly dup2s onto its stdio. Without this, a pipeline started while
    // another is running inherits the other's fds and holds its pipe open, and
    // the reader of that pipe never sees EOF. posix_spawn's adddup2 clears
    // FD_CLOEXEC on the descriptor it creates, so the intended end still passes.
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    // The parent reads without blocking; this is the read end's own file
    // description, so the child's write end stays blocking as it should.
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    p->read_end = fds[0];
    p->write_end = fds[1];
    return 0;
}

void rs_fd_close(rs_fd fd) {
    if (fd >= 0) close(fd);
}

long rs_fd_read_nonblocking(rs_fd fd, void *buf, size_t cap) {
    ssize_t n;
    if (fd < 0 || !buf || cap == 0) return -1;
    do { n = read(fd, buf, cap); } while (n < 0 && errno == EINTR);
    if (n > 0) return (long)n;
    if (n == 0) return -1;                                    // writer closed
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;    // nothing yet
    return -1;
}

static int add_stdio_action(posix_spawn_file_actions_t *fa, const rs_stdio *io, int target) {
    if (!io || io->kind == RS_STDIO_INHERIT) return 0;
    switch (io->kind) {
    case RS_STDIO_NULL:
        return posix_spawn_file_actions_addopen(fa, target, "/dev/null",
                                                target == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);
    case RS_STDIO_FILE:
        return posix_spawn_file_actions_addopen(fa, target, io->path ? io->path : "/dev/null",
                                                O_WRONLY | O_CREAT | O_TRUNC, 0600);
    case RS_STDIO_FD:
        if (io->fd < 0) return EINVAL;
        return posix_spawn_file_actions_adddup2(fa, io->fd, target);
    default:
        return 0;
    }
}

int rs_proc_spawn(rs_proc *out, const char *const *argv, const char *const *envp,
                  const rs_stdio *in, const rs_stdio *out_io, const rs_stdio *err_io,
                  char *errbuf, size_t errlen) {
    posix_spawn_file_actions_t fa;
    pid_t pid = -1;
    int rc;

    if (!out || !argv || !argv[0]) return EINVAL;
    memset(out, 0, sizeof(*out));
    out->pid = -1;

    posix_spawn_file_actions_init(&fa);
    rc = add_stdio_action(&fa, in, STDIN_FILENO);
    if (!rc) rc = add_stdio_action(&fa, out_io, STDOUT_FILENO);
    if (!rc) rc = add_stdio_action(&fa, err_io, STDERR_FILENO);
    if (rc) {
        posix_spawn_file_actions_destroy(&fa);
        if (errbuf && errlen) snprintf(errbuf, errlen, "could not set up the child's stdio: %s", strerror(rc));
        return rc;
    }

    rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv,
                      envp ? (char *const *)envp : environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "could not run \"%s\": %s", argv[0], strerror(rc));
        return rc;
    }
    out->pid = pid;
    out->valid = true;
    return 0;
}

bool rs_proc_valid(const rs_proc *p) { return p && p->valid && p->pid > 0; }
long rs_proc_id(const rs_proc *p)    { return (p && p->valid) ? (long)p->pid : -1; }

static int decode_status(int status, int *exit_code, int *term_signal) {
    if (exit_code)   *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (term_signal) *term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    return 1;
}

int rs_proc_try_wait(rs_proc *p, int *exit_code, int *term_signal) {
    int status = 0;
    pid_t done;
    if (!rs_proc_valid(p)) return -1;
    do { done = waitpid(p->pid, &status, WNOHANG); } while (done < 0 && errno == EINTR);
    if (done == 0) return 0;
    if (done < 0) return -1;
    p->valid = false;
    p->pid = -1;
    return decode_status(status, exit_code, term_signal);
}

int rs_proc_wait(rs_proc *p, double timeout_s, int *exit_code, int *term_signal) {
    if (!rs_proc_valid(p)) return -1;
    if (timeout_s <= 0) {
        int status = 0;
        pid_t done;
        do { done = waitpid(p->pid, &status, 0); } while (done < 0 && errno == EINTR);
        if (done < 0) return -1;
        p->valid = false;
        p->pid = -1;
        return decode_status(status, exit_code, term_signal);
    }
    // Poll rather than sleep on SIGCHLD: this runs on worker threads that must
    // not install process-wide signal handlers.
    for (double waited = 0; waited < timeout_s; waited += 0.005) {
        int r = rs_proc_try_wait(p, exit_code, term_signal);
        if (r != 0) return r;
        struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return 0;
}

void rs_proc_terminate(rs_proc *p) { if (rs_proc_valid(p)) kill(p->pid, SIGTERM); }
void rs_proc_kill(rs_proc *p)      { if (rs_proc_valid(p)) kill(p->pid, SIGKILL); }

void rs_proc_release(rs_proc *p) {
    if (!p) return;
    p->valid = false;
    p->pid = -1;
}

char **rs_env_build(const char *const *keys, const char *const *values, size_t count) {
    size_t inherited = 0, used = 0, i;
    char **out;
    while (environ[inherited]) inherited++;
    out = (char **)calloc(inherited + count + 1, sizeof(char *));
    if (!out) return NULL;
    for (i = 0; i < inherited; i++) {
        const char *eq = strchr(environ[i], '=');
        size_t name_len = eq ? (size_t)(eq - environ[i]) : strlen(environ[i]);
        bool overridden = false;
        size_t k;
        for (k = 0; k < count; k++)
            if (strlen(keys[k]) == name_len && strncmp(keys[k], environ[i], name_len) == 0) {
                overridden = true;
                break;
            }
        if (overridden) continue;
        out[used] = rs_strdup(environ[i]);
        if (!out[used++]) { rs_env_free(out); return NULL; }
    }
    for (i = 0; i < count; i++) {
        size_t need = strlen(keys[i]) + strlen(values[i] ? values[i] : "") + 2;
        out[used] = (char *)malloc(need);
        if (!out[used]) { rs_env_free(out); return NULL; }
        snprintf(out[used++], need, "%s=%s", keys[i], values[i] ? values[i] : "");
    }
    return out;
}

// ===========================================================================
#else  // _WIN32
// ===========================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int rs_pipe_open(rs_pipe *p) {
    SECURITY_ATTRIBUTES sa;
    HANDLE r = NULL, w = NULL;
    if (!p) return -1;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    // Inheritable by default, then narrowed per-spawn by the explicit handle
    // list in rs_proc_spawn — a handle absent from that list is not inherited
    // even though this flag is set.
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&r, &w, &sa, 0)) return -1;
    // The parent's read end must never leak into a child, or the child holds
    // the pipe open and the reader never sees EOF.
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    p->read_end = (rs_fd)r;
    p->write_end = (rs_fd)w;
    return 0;
}

void rs_fd_close(rs_fd fd) {
    if (fd && fd != RS_FD_INVALID) CloseHandle((HANDLE)fd);
}

long rs_fd_read_nonblocking(rs_fd fd, void *buf, size_t cap) {
    HANDLE h = (HANDLE)fd;
    DWORD avail = 0, got = 0;
    if (!h || h == INVALID_HANDLE_VALUE || !buf || cap == 0) return -1;
    // Anonymous pipes have no non-blocking mode, so ask first and only read what
    // is already there. A failed peek means the write end is gone — end of file.
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return -1;
    if (avail == 0) return 0;
    if ((size_t)avail > cap) avail = (DWORD)cap;
    if (!ReadFile(h, buf, avail, &got, NULL)) return -1;
    return got > 0 ? (long)got : -1;
}

// --- command line quoting ---------------------------------------------------
// CreateProcess takes one string, and the child's CRT (or CommandLineToArgvW)
// splits it again. Getting this wrong is how a path with a space, or a header
// value with a quote in it, silently becomes two arguments. These are the rules
// that round-trip through that parser.
static bool arg_needs_quotes(const char *a) {
    if (!*a) return true;                       // an empty argument must be ""
    for (; *a; a++)
        if (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\v' || *a == '"') return true;
    return false;
}

static bool cmdline_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t want = (*cap ? *cap : 256);
        char *grown;
        while (want < *len + n + 1) want *= 2;
        grown = (char *)realloc(*buf, want);
        if (!grown) return false;
        *buf = grown;
        *cap = want;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static char *build_command_line(const char *const *argv) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    size_t i;
    if (!cmdline_append(&buf, &len, &cap, "", 0)) return NULL;
    for (i = 0; argv[i]; i++) {
        const char *a = argv[i];
        if (i && !cmdline_append(&buf, &len, &cap, " ", 1)) goto fail;
        if (!arg_needs_quotes(a)) {
            if (!cmdline_append(&buf, &len, &cap, a, strlen(a))) goto fail;
            continue;
        }
        if (!cmdline_append(&buf, &len, &cap, "\"", 1)) goto fail;
        while (*a) {
            size_t slashes = 0;
            while (*a == '\\') { slashes++; a++; }
            if (!*a) {
                // Trailing backslashes precede the closing quote, so they must
                // be doubled or they would escape it.
                for (; slashes; slashes--)
                    if (!cmdline_append(&buf, &len, &cap, "\\\\", 2)) goto fail;
                break;
            }
            if (*a == '"') {
                for (; slashes; slashes--)
                    if (!cmdline_append(&buf, &len, &cap, "\\\\", 2)) goto fail;
                if (!cmdline_append(&buf, &len, &cap, "\\\"", 2)) goto fail;
            } else {
                for (; slashes; slashes--)
                    if (!cmdline_append(&buf, &len, &cap, "\\", 1)) goto fail;
                if (!cmdline_append(&buf, &len, &cap, a, 1)) goto fail;
            }
            a++;
        }
        if (!cmdline_append(&buf, &len, &cap, "\"", 1)) goto fail;
    }
    return buf;
fail:
    free(buf);
    return NULL;
}

// --- stdio resolution -------------------------------------------------------

static HANDLE open_null_handle(bool for_reading) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    return CreateFileA("NUL", for_reading ? GENERIC_READ : GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

static HANDLE open_file_handle(const char *path) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    return CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

// A console handle cannot go into PROC_THREAD_ATTRIBUTE_HANDLE_LIST —
// CreateProcess rejects the whole call with ERROR_INVALID_PARAMETER. When an
// inherited stream turns out to be the console, hand the child NUL instead:
// nothing in this tree wants a child talking to the server's console, and the
// POSIX side already redirects the one case that mattered.
static bool handle_is_console(HANDLE h) {
    DWORD mode;
    return h && h != INVALID_HANDLE_VALUE &&
           GetFileType(h) == FILE_TYPE_CHAR && GetConsoleMode(h, &mode);
}

// Resolves one stdio slot to an inheritable handle. *owned is set when the
// caller must close it after the spawn.
static HANDLE resolve_stdio(const rs_stdio *io, DWORD std_id, bool for_reading, bool *owned) {
    HANDLE h;
    *owned = false;
    if (!io || io->kind == RS_STDIO_INHERIT) {
        h = GetStdHandle(std_id);
        if (handle_is_console(h) || !h || h == INVALID_HANDLE_VALUE) {
            *owned = true;
            return open_null_handle(for_reading);
        }
        SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        return h;
    }
    switch (io->kind) {
    case RS_STDIO_NULL:
        *owned = true;
        return open_null_handle(for_reading);
    case RS_STDIO_FILE:
        *owned = true;
        return open_file_handle(io->path ? io->path : "NUL");
    case RS_STDIO_FD:
        h = (HANDLE)io->fd;
        if (!h || h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
        SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        return h;
    default:
        return INVALID_HANDLE_VALUE;
    }
}

// The parent's environment plus overrides, as the double-NUL-terminated
// "K=V\0K=V\0\0" block CreateProcess wants.
static char *env_block_from_strv(const char *const *envp) {
    size_t total = 1, len = 0, i;
    char *block, *p;
    for (i = 0; envp[i]; i++) total += strlen(envp[i]) + 1;
    block = (char *)malloc(total < 2 ? 2 : total);
    if (!block) return NULL;
    p = block;
    for (i = 0; envp[i]; i++) {
        len = strlen(envp[i]) + 1;
        memcpy(p, envp[i], len);
        p += len;
    }
    *p = '\0';   // the terminating empty string
    return block;
}

int rs_proc_spawn(rs_proc *out, const char *const *argv, const char *const *envp,
                  const rs_stdio *in, const rs_stdio *out_io, const rs_stdio *err_io,
                  char *errbuf, size_t errlen) {
    STARTUPINFOEXA si;
    PROCESS_INFORMATION pi;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    SIZE_T attr_size = 0;
    HANDLE h_in = INVALID_HANDLE_VALUE, h_out = INVALID_HANDLE_VALUE, h_err = INVALID_HANDLE_VALUE;
    HANDLE inherit[3];
    DWORD  inherit_n = 0, i, err;
    bool own_in = false, own_out = false, own_err = false;
    char *cmdline = NULL, *env_block = NULL;
    int rc = -1;

    if (!out || !argv || !argv[0]) return EINVAL;
    memset(out, 0, sizeof(*out));

    cmdline = build_command_line(argv);
    if (!cmdline) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory building the command line");
        return ENOMEM;
    }
    if (envp) {
        env_block = env_block_from_strv(envp);
        if (!env_block) {
            free(cmdline);
            if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory building the environment");
            return ENOMEM;
        }
    }

    h_in  = resolve_stdio(in,     STD_INPUT_HANDLE,  true,  &own_in);
    h_out = resolve_stdio(out_io, STD_OUTPUT_HANDLE, false, &own_out);
    h_err = resolve_stdio(err_io, STD_ERROR_HANDLE,  false, &own_err);
    if (h_in == INVALID_HANDLE_VALUE || h_out == INVALID_HANDLE_VALUE || h_err == INVALID_HANDLE_VALUE) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "could not set up the child's stdio (error %lu)", GetLastError());
        rc = EINVAL;
        goto done;
    }

    // Exactly these three handles, deduplicated — the same handle listed twice
    // is also rejected.
    {
        HANDLE candidates[3];
        candidates[0] = h_in; candidates[1] = h_out; candidates[2] = h_err;
        for (i = 0; i < 3; i++) {
            DWORD k;
            bool dup = false;
            for (k = 0; k < inherit_n; k++) if (inherit[k] == candidates[i]) { dup = true; break; }
            if (!dup) inherit[inherit_n++] = candidates[i];
        }
    }

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput  = h_in;
    si.StartupInfo.hStdOutput = h_out;
    si.StartupInfo.hStdError  = h_err;

    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherit, (SIZE_T)inherit_n * sizeof(HANDLE), NULL, NULL)) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "could not restrict handle inheritance (error %lu)", GetLastError());
        rc = EINVAL;
        goto done;
    }
    si.lpAttributeList = attrs;

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                        env_block, NULL, &si.StartupInfo, &pi)) {
        err = GetLastError();
        if (errbuf && errlen) {
            if (err == ERROR_FILE_NOT_FOUND)
                snprintf(errbuf, errlen, "could not run \"%s\": not found on PATH", argv[0]);
            else
                snprintf(errbuf, errlen, "could not run \"%s\": Windows error %lu", argv[0], err);
        }
        rc = (err == ERROR_FILE_NOT_FOUND) ? ENOENT : EINVAL;
        goto done;
    }

    CloseHandle(pi.hThread);
    out->handle = pi.hProcess;
    out->id = pi.dwProcessId;
    out->valid = true;
    rc = 0;

done:
    if (attrs) { DeleteProcThreadAttributeList(attrs); free(attrs); }
    if (own_in  && h_in  != INVALID_HANDLE_VALUE) CloseHandle(h_in);
    if (own_out && h_out != INVALID_HANDLE_VALUE) CloseHandle(h_out);
    if (own_err && h_err != INVALID_HANDLE_VALUE) CloseHandle(h_err);
    free(cmdline);
    free(env_block);
    return rc;
}

bool rs_proc_valid(const rs_proc *p) { return p && p->valid && p->handle; }
long rs_proc_id(const rs_proc *p)    { return (p && p->valid) ? (long)p->id : -1; }

int rs_proc_try_wait(rs_proc *p, int *exit_code, int *term_signal) {
    DWORD code = 0;
    if (!rs_proc_valid(p)) return -1;
    if (term_signal) *term_signal = 0;     // Windows has no signals
    if (WaitForSingleObject((HANDLE)p->handle, 0) != WAIT_OBJECT_0) return 0;
    if (!GetExitCodeProcess((HANDLE)p->handle, &code)) return -1;
    if (exit_code) *exit_code = (int)code;
    rs_proc_release(p);
    return 1;
}

int rs_proc_wait(rs_proc *p, double timeout_s, int *exit_code, int *term_signal) {
    DWORD code = 0;
    DWORD ms = (timeout_s <= 0) ? INFINITE
             : (timeout_s > 2147483.0 ? 0x7FFFFFFFu : (DWORD)(timeout_s * 1000.0));
    if (!rs_proc_valid(p)) return -1;
    if (term_signal) *term_signal = 0;
    switch (WaitForSingleObject((HANDLE)p->handle, ms)) {
    case WAIT_OBJECT_0:
        if (!GetExitCodeProcess((HANDLE)p->handle, &code)) return -1;
        if (exit_code) *exit_code = (int)code;
        rs_proc_release(p);
        return 1;
    case WAIT_TIMEOUT:
        return 0;
    default:
        return -1;
    }
}

// There is no SIGTERM for a child that shares no console with us, so both of
// these end the process immediately. rs_proc.h documents the difference.
void rs_proc_terminate(rs_proc *p) { if (rs_proc_valid(p)) TerminateProcess((HANDLE)p->handle, 1); }
void rs_proc_kill(rs_proc *p)      { if (rs_proc_valid(p)) TerminateProcess((HANDLE)p->handle, 1); }

void rs_proc_release(rs_proc *p) {
    if (!p) return;
    if (p->handle) CloseHandle((HANDLE)p->handle);
    p->handle = NULL;
    p->valid = false;
}

char **rs_env_build(const char *const *keys, const char *const *values, size_t count) {
    LPCH env = GetEnvironmentStrings();
    size_t inherited = 0, used = 0, i;
    char **out;
    const char *scan;
    if (!env) return NULL;
    for (scan = env; *scan; scan += strlen(scan) + 1) inherited++;
    out = (char **)calloc(inherited + count + 1, sizeof(char *));
    if (!out) { FreeEnvironmentStringsA(env); return NULL; }
    for (scan = env; *scan; scan += strlen(scan) + 1) {
        const char *eq = strchr(scan, '=');
        size_t name_len = eq ? (size_t)(eq - scan) : strlen(scan);
        bool overridden = false;
        size_t k;
        // Windows environment names are case-insensitive, so an override of
        // "path" must displace "Path" rather than sit beside it.
        for (k = 0; k < count; k++)
            if (strlen(keys[k]) == name_len && _strnicmp(keys[k], scan, name_len) == 0) {
                overridden = true;
                break;
            }
        // The "=C:=C:\..." per-drive cwd entries start with '=' and must survive.
        if (overridden) continue;
        out[used] = rs_strdup(scan);
        if (!out[used++]) { FreeEnvironmentStringsA(env); rs_env_free(out); return NULL; }
    }
    FreeEnvironmentStringsA(env);
    for (i = 0; i < count; i++) {
        size_t need = strlen(keys[i]) + strlen(values[i] ? values[i] : "") + 2;
        out[used] = (char *)malloc(need);
        if (!out[used]) { rs_env_free(out); return NULL; }
        snprintf(out[used++], need, "%s=%s", keys[i], values[i] ? values[i] : "");
    }
    return out;
}

#endif  // _WIN32

// ===========================================================================
// Shared
// ===========================================================================

void rs_env_free(char **env) {
    size_t i;
    if (!env) return;
    for (i = 0; env[i]; i++) free(env[i]);
    free(env);
}

void rs_proc_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static double monotonic_seconds(void) {
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
#endif
}

// A growable capture buffer for one of the child's streams.
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} capture;

// 16 MB is far more than any script or downloader should say, and stops a
// runaway child from taking the server down with it.
#define RS_CAPTURE_MAX (16u * 1024u * 1024u)

static bool capture_push(capture *c, const char *bytes, size_t n) {
    if (c->len + n + 1 > c->cap) {
        size_t want = c->cap ? c->cap : 4096;
        char *grown;
        while (want < c->len + n + 1) want *= 2;
        if (want > RS_CAPTURE_MAX) want = RS_CAPTURE_MAX;
        if (c->len + 1 >= want) return true;                // full: drop the rest
        if (c->len + n + 1 > want) n = want - c->len - 1;   // clamp, keep going
        grown = (char *)realloc(c->data, want);
        if (!grown) return false;
        c->data = grown;
        c->cap = want;
    }
    memcpy(c->data + c->len, bytes, n);
    c->len += n;
    c->data[c->len] = '\0';
    return true;
}

int rs_proc_run(const char *const *argv, const char *const *envp, double timeout_s,
                bool capture_stdout, bool capture_stderr, const char *stderr_path,
                rs_run_result *res, char *errbuf, size_t errlen) {
    rs_pipe out_pipe = { RS_FD_INVALID, RS_FD_INVALID };
    rs_pipe err_pipe = { RS_FD_INVALID, RS_FD_INVALID };
    rs_stdio in_io = rs_stdio_null(), out_io, err_io;
    capture out_cap = {0}, err_cap = {0};
    rs_proc proc;
    bool out_open = false, err_open = false;
    double start;
    int rc;

    if (!res) return -1;
    memset(res, 0, sizeof(*res));

    if (capture_stdout) {
        if (rs_pipe_open(&out_pipe) != 0) {
            if (errbuf && errlen) snprintf(errbuf, errlen, "could not create a pipe");
            return -1;
        }
        out_open = true;
        out_io = rs_stdio_fd(out_pipe.write_end);
    } else {
        out_io = rs_stdio_null();
    }

    if (stderr_path) {
        err_io = rs_stdio_file(stderr_path);
    } else if (capture_stderr) {
        if (rs_pipe_open(&err_pipe) != 0) {
            if (out_open) { rs_fd_close(out_pipe.read_end); rs_fd_close(out_pipe.write_end); }
            if (errbuf && errlen) snprintf(errbuf, errlen, "could not create a pipe");
            return -1;
        }
        err_open = true;
        err_io = rs_stdio_fd(err_pipe.write_end);
    } else {
        err_io = rs_stdio_null();
    }

    rc = rs_proc_spawn(&proc, argv, envp, &in_io, &out_io, &err_io, errbuf, errlen);

    // The parent drops the write ends immediately: while it still holds one, the
    // pipe never reaches end-of-file and the drain loop below would spin until
    // the timeout even after the child has exited.
    if (out_open) { rs_fd_close(out_pipe.write_end); out_pipe.write_end = RS_FD_INVALID; }
    if (err_open) { rs_fd_close(err_pipe.write_end); err_pipe.write_end = RS_FD_INVALID; }

    if (rc != 0) {
        if (out_open) rs_fd_close(out_pipe.read_end);
        if (err_open) rs_fd_close(err_pipe.read_end);
        res->spawn_error = rc;   // ENOENT tells a caller the tool is simply absent
        res->out = rs_strdup("");
        res->err = rs_strdup("");
        return -1;
    }
    res->spawned = true;

    // Drain both streams while waiting. Reading only one and waiting on the
    // child would deadlock the moment the child filled the other pipe.
    start = monotonic_seconds();
    for (;;) {
        char buf[4096];
        bool moved = false;
        long n;
        int exited;

        if (out_open) {
            while ((n = rs_fd_read_nonblocking(out_pipe.read_end, buf, sizeof(buf))) > 0) {
                capture_push(&out_cap, buf, (size_t)n);
                moved = true;
            }
            if (n < 0) { rs_fd_close(out_pipe.read_end); out_pipe.read_end = RS_FD_INVALID; out_open = false; }
        }
        if (err_open) {
            while ((n = rs_fd_read_nonblocking(err_pipe.read_end, buf, sizeof(buf))) > 0) {
                capture_push(&err_cap, buf, (size_t)n);
                moved = true;
            }
            if (n < 0) { rs_fd_close(err_pipe.read_end); err_pipe.read_end = RS_FD_INVALID; err_open = false; }
        }

        exited = rs_proc_try_wait(&proc, &res->exit_code, &res->term_signal);
        if (exited == 1) {
            // One last sweep: bytes written just before exit are still in the
            // pipe buffer, and dropping them loses the error message that
            // explains the exit code.
            if (out_open)
                while ((n = rs_fd_read_nonblocking(out_pipe.read_end, buf, sizeof(buf))) > 0)
                    capture_push(&out_cap, buf, (size_t)n);
            if (err_open)
                while ((n = rs_fd_read_nonblocking(err_pipe.read_end, buf, sizeof(buf))) > 0)
                    capture_push(&err_cap, buf, (size_t)n);
            break;
        }
        if (exited < 0) break;

        if (timeout_s > 0 && monotonic_seconds() - start >= timeout_s) {
            res->timed_out = true;
            rs_proc_terminate(&proc);
            if (rs_proc_wait(&proc, 0.5, &res->exit_code, &res->term_signal) != 1) {
                rs_proc_kill(&proc);
                rs_proc_wait(&proc, 0, &res->exit_code, &res->term_signal);
            }
            break;
        }
        if (!moved) rs_proc_sleep_ms(5);
    }

    rs_proc_release(&proc);
    if (out_open) rs_fd_close(out_pipe.read_end);
    if (err_open) rs_fd_close(err_pipe.read_end);

    res->out = out_cap.data ? out_cap.data : rs_strdup("");
    res->err = err_cap.data ? err_cap.data : rs_strdup("");
    return 0;
}

void rs_run_result_dispose(rs_run_result *res) {
    if (!res) return;
    free(res->out);
    free(res->err);
    res->out = NULL;
    res->err = NULL;
}

size_t rs_proc_interpreter_for(const char *script_path, const char **prefix, size_t max) {
    const char *dot;
    const char *ext = "";
    if (!script_path || !prefix || max == 0) return 0;
    dot = strrchr(script_path, '.');
    if (dot) ext = dot + 1;

#ifdef _WIN32
    if (_stricmp(ext, "py") == 0) {
        // "python3" is not a thing on Windows outside the Store stub; the
        // real installers put python.exe on PATH.
        prefix[0] = "python";
        return 1;
    }
    if (_stricmp(ext, "bat") == 0 || _stricmp(ext, "cmd") == 0) {
        if (max < 2) return 0;
        prefix[0] = "cmd.exe";
        prefix[1] = "/c";
        return 2;
    }
    if (_stricmp(ext, "ps1") == 0) {
        if (max < 5) return 0;
        prefix[0] = "powershell.exe";
        prefix[1] = "-NoProfile";
        prefix[2] = "-ExecutionPolicy";
        prefix[3] = "Bypass";
        prefix[4] = "-File";   // so the caller's appended path is run, not evaluated
        return 5;
    }
    if (_stricmp(ext, "sh") == 0 || _stricmp(ext, "bash") == 0) {
        // Git for Windows and the WSL shims both put sh.exe on PATH; when
        // neither is installed the spawn fails with a clear "not found".
        prefix[0] = "sh";
        return 1;
    }
#else
    if (strcasecmp(ext, "py") == 0) { prefix[0] = "python3"; return 1; }
    if (strcasecmp(ext, "sh") == 0 || strcasecmp(ext, "bash") == 0) { prefix[0] = "/bin/sh"; return 1; }
#endif
    return 0;   // directly executable
}
