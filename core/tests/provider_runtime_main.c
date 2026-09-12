// Deterministic lifecycle checks use this executable as the supervised child.
#include "ffrun.h"
#include "epg.h"
#include "rs_proc.h"
#include "rs_provider_options.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, starts, halted, stalls;
static void check(const char *name, int ok) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", name); failures++; }
}
static void log_event(void *ctx, const char *sid, const char *level, const char *event, const char *message) {
    (void)ctx; (void)sid; (void)level; (void)message;
    if (!strcmp(event, "ffmpegStart")) starts++;
    if (!strcmp(event, "ffmpegHalted")) halted++;
    if (!strcmp(event, "ffmpegStalled")) stalls++;
}
static void poll_for(rs_ffrun *runner, int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 20) { rs_ffrun_poll(runner); rs_proc_sleep_ms(20); }
}
int main(int argc, char **argv) {
    if (argc > 1) {
        if (!strcmp(argv[1], "error")) return 1;
        if (!strcmp(argv[1], "finished")) return 0;
        if (!strcmp(argv[1], "stalled")) { rs_proc_sleep_ms(10000); return 0; }
        if (!strcmp(argv[1], "progress")) {
            for (int i = 0; i < 20; i++) { fprintf(stderr, "out_time_us=%d\n", i * 200000); fflush(stderr); rs_proc_sleep_ms(200); }
            return 0;
        }
    }
    rs_source_policy policy = {0};
    policy.no_restart_error = 1; policy.restart_delay = 1;
    const char *command[] = {argv[0], "error", NULL};
    rs_ffrun *runner = rs_ffrun_create(log_event, NULL);
    check("start child", rs_ffrun_start(runner, "test", command, NULL, NULL, NULL, 0, &policy) == 0);
    poll_for(runner, 1300);
    check("no restart after error", starts == 1 && halted == 1);
    rs_ffrun_stop(runner, "test"); starts = halted = 0;
    policy.no_restart_error = 0; policy.cooldown = 1;
    rs_ffrun_start(runner, "test", command, NULL, NULL, NULL, 0, &policy);
    poll_for(runner, 2400);
    check("restart delay and exponential cooldown", starts == 2 && halted == 0);
    rs_ffrun_stop(runner, "test"); starts = halted = 0;
    command[1] = "finished";
    rs_ffrun_start(runner, "test", command, NULL, NULL, NULL, 0, &policy);
    poll_for(runner, 1300);
    check("finished broadcasts remain stopped", starts == 1 && halted == 1);
    rs_ffrun_stop(runner, "test"); starts = halted = 0;
    policy.restart_finished = 1;
    rs_ffrun_start(runner, "test", command, NULL, NULL, NULL, 0, &policy);
    poll_for(runner, 1300);
    check("restart finished broadcast", starts == 2 && halted == 0);
    rs_ffrun_stop(runner, "test"); starts = halted = stalls = 0;
    policy.no_restart_error = 1; policy.stalled_seconds = 1;
    command[1] = "stalled";
    rs_ffrun_start(runner, "test", command, NULL, NULL, NULL, 0, &policy);
    poll_for(runner, 1300);
    check("stalled child terminated without restart", starts == 1 && halted == 1 && stalls == 1);
    rs_ffrun_stop(runner, "test"); starts = halted = stalls = 0;
    command[1] = "progress";
    rs_ffrun_start(runner, "test", command, NULL, NULL, NULL, 0, &policy);
    poll_for(runner, 1800);
    check("advancing output prevents false stall", starts == 1 && halted == 0 && stalls == 0);
    rs_ffrun_stop(runner, "test");
    const char *missing[] = {"/restreamair-test-no-such-executable", NULL};
    check("failed spawn does not consume a slot with retries disabled",
          rs_ffrun_start(runner, "missing", missing, NULL, NULL, NULL, 0, &policy) != 0 &&
          !rs_ffrun_is_running(runner, "missing"));
    rs_ffrun_destroy(runner);

    int offset = 0;
    check("timezone offset", rs_provider_timezone_offset("UTC+05:30", &offset) && offset == 330);
    check("timezone validation", !rs_provider_timezone_offset("UTC+14:01", &offset) && !rs_provider_timezone_offset("Europe/London", &offset));
    const char *xml = "<tv><programme start='20261231230000 +0000' stop='20270101060000 +0500' channel='x'><title>A &amp; B</title></programme></tv>";
    char err[256] = {0};
    char *converted = rs_epg_timezone(xml, strlen(xml), 330, err, sizeof(err));
    check("EPG preserves instants across year boundary", converted && strstr(converted, "20270101043000 +0530") && strstr(converted, "20270101063000 +0530"));
    check("EPG preserves text", converted && strstr(converted, "A &amp; B"));
    free(converted);
    if (failures) return 1;
    puts("provider runtime: all lifecycle and EPG checks PASS");
    return 0;
}
