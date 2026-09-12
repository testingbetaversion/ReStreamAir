#ifndef RS_SOURCE_POLICY_H
#define RS_SOURCE_POLICY_H

// Owned, immutable request configuration. Safe to copy into background jobs.
typedef struct {
    char provider_id[128];
    char cookie_file[256];
    char video_filter[4097], audio_filter[4097];
    int timeout_seconds, attempts, max_downloads;
    int use_cookies, detect_json_redirect, legacy_dash;
    int no_restart_error, restart_finished, no_restart_track, cooldown;
    int restart_delay, stalled_seconds, manifest_retries, ignore_static, use_dash_delay;
} rs_source_policy;

#endif
