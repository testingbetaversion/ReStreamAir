#ifndef RS_SERVICE_H
#define RS_SERVICE_H

// systemd integration for the Settings page.
//
// Two things the panel could not do for itself. The Panel port setting says it
// "takes effect after restart", so acting on it meant leaving the browser and
// finding a shell — and on a fresh install there was often no unit at all, just
// a binary someone had started by hand, which does not survive a reboot. Both
// are now buttons in Settings.
//
// Deliberately not a general "run a command" endpoint. Every operation here is
// a fixed argv with a validated unit name, so an authenticated admin can
// restart or install *this* service and nothing else. The panel already
// requires admin auth for /api/*; this adds no new way to reach a shell.
//
// POSIX only. On a platform without systemd, rs_service_probe reports it and
// the panel hides the controls rather than offering something that cannot work.

#include "rs_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool systemd_available;  // systemctl is present and usable
    bool unit_installed;     // a unit by this name exists
    bool active;             // currently running under systemd
    bool enabled;            // starts on boot
    bool can_manage;         // we have the privileges to act (root)
    char *unit_name;         // e.g. "restreamair.service"
    char *unit_path;         // where the unit file lives, when installed
    char *exec_start;        // what the installed unit launches
    char *self_path;         // this binary's absolute path, for the install form
    char *working_dir;       // this process's cwd, ditto
} rs_service_info;

// Fills `out` with what can be determined about the service. Never fails hard:
// an absent systemd is a reported state, not an error. `unit_name` may be NULL
// for the default. Release with rs_service_info_dispose.
void rs_service_probe(const char *unit_name, rs_service_info *out);
void rs_service_info_dispose(rs_service_info *info);

// Hands a restart to systemd and returns immediately (systemctl --no-block), so
// the caller can reply before this process is replaced. Returns 0 when systemd
// accepted the request. The caller is responsible for having already flushed
// its response — see the deferred restart in server.c.
int rs_service_restart(const char *unit_name, char *errbuf, size_t errlen);

// Writes a unit for the *currently running* binary and working directory, then
// daemon-reloads and enables it. Generated from reality rather than copied from
// the repo template, so the paths are the ones actually in use. `port` and
// `bind` go into ExecStart; `bind` may be NULL. Returns 0 on success.
int rs_service_install(const char *unit_name, unsigned port, const char *bind,
                       char *errbuf, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif  // RS_SERVICE_H
