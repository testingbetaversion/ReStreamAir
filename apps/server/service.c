#include "service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#define RS_SERVICE_DEFAULT_UNIT "restreamair.service"
#define RS_SERVICE_UNIT_DIR "/etc/systemd/system"

// A unit name reaches systemctl as an argument, so it is constrained to what a
// unit name may actually contain. Nothing here is passed through a shell, but a
// name with a slash in it could still point systemctl at another directory.
static bool unit_name_ok(const char *name) {
    if (!name || !name[0] || strlen(name) > 96) return false;
    for (const char *p = name; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '@';
        if (!ok) return false;
    }
    // No traversal, and it must look like a unit rather than a path.
    if (strstr(name, "..")) return false;
    return true;
}

#ifndef _WIN32

// Runs argv, capturing stdout. Returns the exit status, or -1 if it could not be
// started. `out` (optional) receives up to `out_cap-1` bytes, NUL-terminated.
static int run_capture(const char *const *argv, char *out, size_t out_cap) {
    if (out && out_cap) out[0] = '\0';
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) return -1;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) { close(fds[0]); return -1; }

    size_t used = 0;
    char buf[512];
    for (;;) {
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) break;
        if (out && used + (size_t)n < out_cap) {
            memcpy(out + used, buf, (size_t)n);
            used += (size_t)n;
            out[used] = '\0';
        }
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Strips one trailing newline, which every systemctl one-word answer has.
static void chomp(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

static char *self_exe_path(void) {
#if defined(__linux__)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return rs_strdup(buf); }
#elif defined(__APPLE__)
    // Not the install target, but the field is still shown in the panel.
    char buf[4096];
    uint32_t cap = sizeof(buf);
    extern int _NSGetExecutablePath(char *, uint32_t *);
    if (_NSGetExecutablePath(buf, &cap) == 0) return rs_strdup(buf);
#endif
    return NULL;
}

void rs_service_probe(const char *unit_name, rs_service_info *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    const char *unit = (unit_name && unit_name_ok(unit_name)) ? unit_name : RS_SERVICE_DEFAULT_UNIT;
    out->unit_name = rs_strdup(unit);
    out->self_path = self_exe_path();

    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) out->working_dir = rs_strdup(cwd);

    // systemd present? `systemctl --version` succeeding is the cheap check, and
    // it also fails correctly inside a container where systemd is not PID 1.
    const char *ver[] = {"systemctl", "--version", NULL};
    out->systemd_available = run_capture(ver, NULL, 0) == 0;
    if (!out->systemd_available) return;

    out->can_manage = geteuid() == 0;

    char path[4200];
    snprintf(path, sizeof(path), "%s/%s", RS_SERVICE_UNIT_DIR, unit);
    struct stat sb;
    if (stat(path, &sb) == 0 && S_ISREG(sb.st_mode)) {
        out->unit_installed = true;
        out->unit_path = rs_strdup(path);
    }

    char answer[256];
    const char *act[] = {"systemctl", "is-active", unit, NULL};
    run_capture(act, answer, sizeof(answer));
    chomp(answer);
    out->active = strcmp(answer, "active") == 0;

    const char *en[] = {"systemctl", "is-enabled", unit, NULL};
    run_capture(en, answer, sizeof(answer));
    chomp(answer);
    out->enabled = strcmp(answer, "enabled") == 0;

    // What the installed unit actually launches, which is the interesting half
    // when the panel's port and the unit's --port disagree.
    const char *show[] = {"systemctl", "show", "-p", "ExecStart", "--value", unit, NULL};
    if (run_capture(show, answer, sizeof(answer)) == 0) {
        chomp(answer);
        if (answer[0]) out->exec_start = rs_strdup(answer);
    }
}

int rs_service_restart(const char *unit_name, char *errbuf, size_t errlen) {
    const char *unit = (unit_name && unit_name_ok(unit_name)) ? unit_name : RS_SERVICE_DEFAULT_UNIT;
    if (errbuf && errlen) errbuf[0] = '\0';
    if (geteuid() != 0) {
        snprintf(errbuf, errlen, "restarting the service needs root");
        return -1;
    }
    // --no-block so systemctl returns before systemd stops this process; the
    // caller has already flushed its reply by the time this runs.
    const char *argv[] = {"systemctl", "restart", "--no-block", unit, NULL};
    char out[512];
    int rc = run_capture(argv, out, sizeof(out));
    if (rc != 0) {
        chomp(out);
        snprintf(errbuf, errlen, "systemctl restart failed: %s", out[0] ? out : "unknown error");
        return -1;
    }
    return 0;
}

int rs_service_install(const char *unit_name, unsigned port, const char *bind,
                       char *errbuf, size_t errlen) {
    const char *unit = (unit_name && unit_name_ok(unit_name)) ? unit_name : RS_SERVICE_DEFAULT_UNIT;
    if (errbuf && errlen) errbuf[0] = '\0';
    if (geteuid() != 0) {
        snprintf(errbuf, errlen, "installing a unit needs root");
        return -1;
    }
    if (port == 0 || port > 65535) {
        snprintf(errbuf, errlen, "invalid port");
        return -1;
    }
    if (bind && strchr(bind, '\n')) {   // no smuggling extra unit directives
        snprintf(errbuf, errlen, "invalid bind address");
        return -1;
    }

    char *exe = self_exe_path();
    char cwd[4096];
    if (!exe || !getcwd(cwd, sizeof(cwd))) {
        free(exe);
        snprintf(errbuf, errlen, "could not determine this binary's path");
        return -1;
    }

    // Generated from the running process rather than copied from the repo
    // template: the template assumes /usr/local/bin and a dedicated user, while
    // what an operator wants installed is whatever is already working. Run as
    // the current user for the same reason — changing ownership underneath a
    // working install is how state.json ends up unreadable.
    char bindarg[256] = {0};
    if (bind && bind[0]) snprintf(bindarg, sizeof(bindarg), " --bind %s", bind);

    char body[8192];
    int n = snprintf(body, sizeof(body),
        "# Generated by ReStreamAir from the running process.\n"
        "# Paths and flags below are the ones that were in use when this was\n"
        "# installed from Settings; edit and `systemctl daemon-reload` to change.\n"
        "[Unit]\n"
        "Description=ReStreamAir DASH/HLS restreaming panel\n"
        "Documentation=https://github.com/testingbetaversion/ReStreamAir\n"
        "After=network-online.target\n"
        "Wants=network-online.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "User=%s\n"
        "WorkingDirectory=%s\n"
        "ExecStart=%s serve --port %u%s\n"
        "Restart=on-failure\n"
        "RestartSec=5s\n"
        "# state.json carries password hashes and script-account passwords.\n"
        "UMask=0077\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        geteuid() == 0 ? "root" : "restreamair", cwd, exe, port, bindarg);
    free(exe);
    if (n <= 0 || (size_t)n >= sizeof(body)) {
        snprintf(errbuf, errlen, "unit file too long");
        return -1;
    }

    char path[4200];
    snprintf(path, sizeof(path), "%s/%s", RS_SERVICE_UNIT_DIR, unit);
    // 0644: systemd must read it, and it carries no secrets.
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(errbuf, errlen, "could not write %s", path);
        return -1;
    }
    ssize_t wrote = write(fd, body, (size_t)n);
    close(fd);
    if (wrote != n) {
        snprintf(errbuf, errlen, "could not write %s", path);
        return -1;
    }

    char out[512];
    const char *reload[] = {"systemctl", "daemon-reload", NULL};
    if (run_capture(reload, out, sizeof(out)) != 0) {
        chomp(out);
        snprintf(errbuf, errlen, "daemon-reload failed: %s", out[0] ? out : "unknown error");
        return -1;
    }
    const char *enable[] = {"systemctl", "enable", unit, NULL};
    if (run_capture(enable, out, sizeof(out)) != 0) {
        chomp(out);
        snprintf(errbuf, errlen, "enable failed: %s", out[0] ? out : "unknown error");
        return -1;
    }
    return 0;
}

#else  // _WIN32

void rs_service_probe(const char *unit_name, rs_service_info *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->unit_name = rs_strdup(unit_name && unit_name[0] ? unit_name : RS_SERVICE_DEFAULT_UNIT);
}
int rs_service_restart(const char *unit_name, char *errbuf, size_t errlen) {
    (void)unit_name;
    if (errbuf && errlen) snprintf(errbuf, errlen, "systemd is not available on this platform");
    return -1;
}
int rs_service_install(const char *unit_name, unsigned port, const char *bind,
                       char *errbuf, size_t errlen) {
    (void)unit_name; (void)port; (void)bind;
    if (errbuf && errlen) snprintf(errbuf, errlen, "systemd is not available on this platform");
    return -1;
}

#endif

void rs_service_info_dispose(rs_service_info *info) {
    if (!info) return;
    rs_free(info->unit_name);
    rs_free(info->unit_path);
    rs_free(info->exec_start);
    rs_free(info->self_path);
    rs_free(info->working_dir);
    memset(info, 0, sizeof(*info));
}
